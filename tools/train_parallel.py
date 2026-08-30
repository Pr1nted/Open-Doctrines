#!/usr/bin/env python3
"""Run several AI training workers at once and merge what they learn.

    tools/train_parallel.py [--workers N] [--turns T] [--limit PCT] [--] [extra args]

Each worker is an ordinary `--train-ai` process with its own model file
(data/ai/model.wN.bin) and its own map seeds. Workers pull a third of the way
toward the average of their peers every two minutes, so nobody blocks on anybody
and a worker that dies costs only its own progress. On exit the surviving models
are merged into data/ai/model.bin, which is the file the game loads.

Why processes and not threads: raylib allows one window per process, map loading
touches the GL context, and a world was measured at about 3 GB resident. On a
16 GB machine that is three workers, so the ceiling is memory long before it is
cores — which also means the default here is deliberately conservative.

Stop with Ctrl-C. The merge still runs.
"""

import argparse
import math
import os
import random
import struct
import shutil
import signal
import subprocess
import sys
import time


# ── Population-based training ──────────────────────────────────────────────
#
# PPO_ENTROPY was set by hand, then measured, then set back by hand -- a
# controlled A/B that cost eighty minutes of training and fifty of evaluation to
# move one number. With several workers already running side by side, the pool
# IS a population: give each worker its own hyperparameters, and periodically
# let the ones that are losing copy the ones that are winning and jitter from
# there. Same wall-clock, and the search happens for free.
#
# Fitness is the worker's own running reward mean, which is serialised in the
# model file (see saveModel's reward-statistics blob). It is the only per-worker
# quality signal available without stopping to run an evaluation, and it is
# comparable across workers because every worker optimises the same reward.
# Seconds between exploit/explore rounds. Overridable because a 30-minute
# interval is untestable: a first run assigned per-worker hyperparameters
# correctly and then fired ZERO rounds in seven and a half hours, and there was
# no way to check the loop short of another seven-hour run.
PBT_INTERVAL = int(os.environ.get("OD_PBT_INTERVAL", "1800"))
PBT_HYPERS = {
    # name -> (default, low, high). Sampled log-uniformly around the default.
    "OD_PPO_ENTROPY": (0.01, 0.003, 0.05),
    "OD_N_STEP":      (12,   6,     24),
}


def _sample_hypers(rng, w):
    """Worker 0 always runs the defaults, so the population keeps a control."""
    env = {}
    for name, (dflt, lo, hi) in PBT_HYPERS.items():
        if w == 0:
            env[name] = dflt
            continue
        lo_l, hi_l = math.log(lo), math.log(hi)
        val = math.exp(rng.uniform(lo_l, hi_l))
        env[name] = int(round(val)) if name == "OD_N_STEP" else round(val, 5)
    return env


def _perturb(rng, hypers):
    """Explore: nudge by a factor, clamped to the declared range."""
    out = {}
    for name, val in hypers.items():
        dflt, lo, hi = PBT_HYPERS[name]
        val = float(val) * rng.choice([0.8, 1.25])
        val = max(lo, min(hi, val))
        out[name] = int(round(val)) if name == "OD_N_STEP" else round(val, 5)
    return out


def _fitness(path, binary=None, seed=4242):
    """How well this worker actually PLAYS, as land share against the scripted rung.

    The previous measure was the mean of the model's own running reward means,
    and it could not work. In self-play the reward is near zero-sum, so the world
    mean sits at zero for any policy; it reflects the maps a worker happened to
    draw; and because the reward is largely penalties -- bankruptcy, provinces
    lost, war weariness -- a policy that does NOTHING avoids all of them and
    scores well. Measured on 2026-08-06 it ranked the worst model on disk first
    (ADVANTAGE 0.545, fitness 0.221) and the best nearly last (ADVANTAGE 2.347,
    fitness 0.061), and per-worker values flipped sign 12, 6 and 7 times across
    16 rounds. PBT was copying weights from an essentially random worker.

    LAND SHARE, NOT ADVANTAGE. The eval also prints an ADVANTAGE ratio, and that
    is unusable here: it is unbounded, so a dominant model's reading explodes --
    identical weights measured 2.347, 5.943 and 7.680 on separate runs. Share is
    bounded in [0, 1] and behaves.

    Returns None if the model cannot be read or the eval produced nothing, which
    the caller already treats as "skip this worker".
    """
    if binary is None or not os.path.exists(path):
        return None
    # Point the eval at THIS worker's file. --eval-ai reads it read-only, so a
    # ranking round cannot disturb the workers that are still training.
    env = dict(os.environ)
    env["OD_EVAL_MODEL"] = path
    try:
        out = subprocess.run(
            _gated([binary, "--resource-limit", "25", "--eval-ai", "1", "150", str(seed), "2",
             "--scenarios", "--vs-script"]),
            cwd=ROOT, env=env, capture_output=True, text=True, timeout=600).stdout
    except (subprocess.SubprocessError, OSError) as e:
        # SAID OUT LOUD. Swallowing this printed "bench did not produce a
        # result" and nothing else, which is indistinguishable from a model that
        # simply could not be read -- and left a regression check that had
        # quietly stopped checking.
        print(f"[POOL] bench subprocess failed: {e}")
        return None
    for line in out.splitlines():
        if line.startswith("[EVAL] land held"):
            try:
                return float(line.split("%")[0].split()[-1]) / 100.0
            except (ValueError, IndexError):
                print(f"[POOL] bench could not parse: {line!r}")
                return None
    print("[POOL] bench produced no 'land held' line; "
          f"eval wrote {len(out.splitlines())} line(s)")
    return None


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data", "ai")

# Leave enough GB for the OS and whatever else the machine is doing, or the
# kernel starts killing workers mid-map and the run silently becomes a slower
# single-worker run.
#
# ── SIZED BY THE PEAK, NOT BY THE QUIET NUMBER ──
#
# The history matters here, because both of the previous numbers were arrived
# at honestly and both were wrong in the same way.
#
# It was 3.0 originally, because a loaded world WAS about 3 GB resident. Then
# the land/sea pixel buffer and four of the five resource buffers stopped being
# kept after load (LandSeaMap::dropPixels, Game_Loading.cpp), taking ~636 MB off
# every world, and workers were re-measured at 0.4-0.6 GB each. So it was cut to
# 0.7 and the reserve to 4.0, with the note that memory had stopped being the
# binding constraint.
#
# On 2026-08-28 that arithmetic auto-picked three workers on a 16 GB Mac and
# took the user's whole machine out of memory.
#
# Both measurements were of a worker AT REST. Sampled every 15 seconds through a
# later run, a worker sits at 0.29-0.41 GB between maps -- and peaks at
# **2.70 GB** while a map is loaded or generated, which is very nearly the
# original 3 GB. dropPixels reduced what a world COSTS TO KEEP; it did not
# reduce what one costs to BUILD. Workers rotate maps on their own seeds, so
# nothing staggers those spikes and three can arrive together: 8.1 GB of peak on
# top of an IDE and a browser is a 16 GB machine gone. A pool sized on the
# resting figure looks harmless for minutes at a time and then all of it lands
# at once.
#
# So the per-worker figure is the peak, and the reserve is what a machine
# somebody is actually WORKING on needs -- an IDE and a browser were using about
# 3.5 GB while this was measured, and the failure mode is not a slow run, it is
# the desktop dying. On 16 GB this yields two workers, measured at 56% of memory
# still free.
#
# Note this still does NOT mean more workers is faster. --limit is a share of
# the MACHINE divided among them (per_worker_limit below), so doubling the
# workers halves what each gets and buys diversity, not throughput.
GB_PER_WORKER = 3.0
# How much land the merge may give up against a rusher before it is rejected,
# even though it beat its own predecessor head-to-head. See _keep_the_winner.
# Five points is wider than the noise on a two-seed bench and narrower than the
# seven-point regression that went unnoticed for five hours of training.
BLITZ_REGRESSION_MARGIN = 0.05

# How often the pool stops to estimate what the shared model is currently worth.
#
# Training reports that the reward went up, which is a statement about the
# reward. This runs tools/od_bench.py --quick against the live model.bin and
# leaves the number in data/ai/bench_score.txt, where the training window picks
# it up -- so a run that is quietly going backwards is visible while it happens
# rather than in a bench afterwards. Half an hour because the estimate costs a
# minute of one core and the thing it measures moves slowly.
def _gated(cmd):
    """Run a game process under the global concurrency gate.

    The pool used to launch a worker, a periodic bench and an exploit eval
    concurrently; each spikes ~2 GB loading the map, which is what put a
    16 GB machine into swap. See tools/odlock.py.
    """
    return [sys.executable, os.path.join(ROOT, "tools", "odlock.py"), "--"] + list(cmd)


BENCH_EVERY_SECONDS = 1800

GB_RESERVED = 8.0

# ── A HARD CEILING ON WHAT A WORKER MAY HOLD ──
#
# The sizing above is an ESTIMATE made before anything runs, and estimates do
# not stop a machine dying: on 2026-08-30 a 16 GB laptop reached 97% swap and
# froze every application on it, with two workers plus a benchmark game and the
# test suite all holding a map at once. Nothing in the pool noticed, because
# nothing was watching.
#
# So the pool now watches. A worker over this ceiling is stopped and restarted
# -- its model is checkpointed to model.w<N>.bin every 60 seconds, so a restart
# costs a minute of training and nothing else, which is a trade worth making
# every time against taking the desktop down with it.
#
# 0 disables the check. --max-rss-gb overrides it for one run.
MAX_WORKER_RSS_GB = 3.0
# How often to look. Cheap (one ps per worker) and the failure it prevents
# develops over minutes, not seconds.
RSS_POLL_SECONDS = 30


def worker_rss_gb(pid):
    """Resident size of a process in GB, or 0.0 if it cannot be read."""
    try:
        out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)],
                             capture_output=True, text=True, timeout=10).stdout.strip()
        return (int(out) / 1024.0 / 1024.0) if out else 0.0
    except (subprocess.SubprocessError, OSError, ValueError):
        return 0.0


def build_type(tree):
    """CMAKE_BUILD_TYPE for a build tree, or "" if it is unset or unreadable.

    An unset type is not a neutral default: CMake passes no -O flag at all, so
    the tree is an unoptimised build that looks exactly like any other.
    """
    cache = os.path.join(tree, "CMakeCache.txt")
    try:
        with open(cache) as f:
            for line in f:
                if line.startswith("CMAKE_BUILD_TYPE:"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return ""


OPTIMISED = ("Release", "RelWithDebInfo", "MinSizeRel")


def find_binary(explicit=None):
    """An OPTIMISED built binary, preferring the newest of those.

    NEWEST ALONE IS THE WRONG RULE, and it cost a night to find out. Taking the
    newest avoids the stale-binary trap, which is real -- training against
    month-old rules looks healthy and optimises a game that no longer exists.
    But an IDE rebuilds its own tree constantly in the background, so
    cmake-build-debug is permanently the newest thing on the disk whatever else
    was built, and its CMAKE_BUILD_TYPE is empty.

    Measured 2026-08-28: a pool started immediately after a verified Release
    build was found to be running cmake-build-debug -- a 16.5 MB unoptimised
    binary against the 6.4 MB Release one -- because the IDE had touched it
    thirty seconds earlier. It was also built from a source state that predated
    the change the run existed to test, so the run was measuring the previous
    version of the code. Both failures are invisible: the pool starts, the
    workers report, the numbers look plausible.

    So: prefer trees that are actually optimised, take the newest of those, and
    say out loud which one and why. --binary overrides all of it.
    """
    if explicit:
        p = explicit if os.path.isabs(explicit) else os.path.join(ROOT, explicit)
        if not os.path.exists(p):
            sys.exit(f"--binary {explicit}: no such file")
        print(f"[pool] binary: {os.path.relpath(p, ROOT)} (explicit --binary)")
        return p

    found = []
    for d in ("cmake-build-debug", "build", "build-release"):
        for rel in ("OpenDoctrines.app/Contents/MacOS/OpenDoctrines",
                    "OpenDoctrines", "OpenDoctrines.exe"):
            p = os.path.join(ROOT, d, rel)
            if os.path.exists(p):
                found.append((os.path.getmtime(p), p, build_type(os.path.join(ROOT, d))))
    if not found:
        sys.exit("No built binary found. Run: cmake --build build -j8")
    found.sort()

    opt = [f for f in found if f[2] in OPTIMISED]
    if opt:
        newest = opt[-1][1]
        skipped = [os.path.relpath(f[1], ROOT) for f in found if f not in opt]
        print(f"[pool] binary: {os.path.relpath(newest, ROOT)} ({opt[-1][2]})")
        if skipped:
            print(f"[pool] ignored {len(skipped)} unoptimised tree(s): {', '.join(skipped)}")
    else:
        newest = found[-1][1]
        print(f"[pool] WARNING: no optimised build tree found. Using "
              f"{os.path.relpath(newest, ROOT)} with CMAKE_BUILD_TYPE="
              f"'{found[-1][2] or 'unset'}'.")
        print(f"[pool] An unoptimised trainer is several times slower for "
              f"nothing. Build one with:")
        print(f"[pool]   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && "
              f"cmake --build build -j8")

    # ...AND THE HEADLESS ONE FROM THAT TREE, IF IT IS BUILT.
    #
    # Every mode of the game binary goes through a window, and InitWindow dies
    # when the display has slept. Measured 2026-08-24: a pool launched at night
    # got worker 0 up, then the screen went off and workers 1 and 2 both died at
    # startup with "GLFW: Failed to determine Monitor to center Window". The
    # pool went on running at a third of its width for as long as nobody looked,
    # and worker 0 spent the night trying to peer-sync against two model files
    # that were never written.
    #
    # OpenDoctrinesServer is the same Game::runAITraining with no renderer
    # linked (see src/server/ServerMain.cpp), so the question cannot arise. An
    # overnight run is exactly the case that must not depend on the screen
    # staying awake, which is also why caffeinate is the wrong answer: it makes
    # the run depend on nobody having changed their power settings.
    tree = os.path.dirname(newest)
    if tree.endswith(os.path.join("OpenDoctrines.app", "Contents", "MacOS")):
        tree = os.path.abspath(os.path.join(tree, "..", "..", ".."))
    server = os.path.join(tree, "OpenDoctrinesServer")
    if os.path.exists(server):
        if os.path.getmtime(server) < os.path.getmtime(newest) - 300:
            print(f"[pool] warning: {os.path.relpath(server, ROOT)} is older than "
                  f"the game binary; rebuild it or the pool trains stale rules.")
        print(f"[pool] headless: {os.path.relpath(server, ROOT)}")
        return server
    print(f"[pool] warning: no OpenDoctrinesServer beside {os.path.relpath(newest, ROOT)};\n"
          f"[pool] using the windowed binary, whose workers DIE if the display sleeps.\n"
          f"[pool] build it with: cmake --build {os.path.relpath(tree, ROOT)} "
          f"--target OpenDoctrinesServer")
    return newest


def machine_gb():
    try:
        if sys.platform == "darwin":
            out = subprocess.check_output(["sysctl", "-n", "hw.memsize"])
            return int(out) / (1024 ** 3)
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    return int(line.split()[1]) / (1024 ** 2)
    except Exception:
        pass
    return 16.0


def safe_worker_count():
    by_mem = int((machine_gb() - GB_RESERVED) / GB_PER_WORKER)
    by_cpu = max(1, (os.cpu_count() or 4) // 3)  # each worker also runs learning threads
    return max(1, min(by_mem, by_cpu, 8))



def _head_to_head(a, b, binary, turns=200, seed=20260801):
    """Land share for model `a` when it plays model `b`, or None.

    BOTH WAYS, AVERAGED, by the caller. The eval splits the map into a model
    cohort and an opponent cohort matched by starting size, but the two halves
    of a scenario are not equally winnable -- measured on 1914/1918, two
    NEARLY IDENTICAL models split 1.17x and 0.85x in the same direction. Running
    one direction and believing it is how a wash gets reported as a 30% gain.
    """
    env = dict(os.environ)
    env["OD_EVAL_MODEL"] = a
    try:
        out = subprocess.run(
            _gated([binary, "--eval-ai", "2", str(turns), str(seed), "2",
             "--scenarios", "--vs-model", b]),
            cwd=ROOT, env=env, capture_output=True, text=True, timeout=3600).stdout
    except (subprocess.SubprocessError, OSError):
        return None
    for line in out.splitlines():
        if line.startswith("[EVAL] land held"):
            try:
                return float(line.split("%")[0].split()[-1]) / 100.0
            except (ValueError, IndexError):
                return None
    return None


def _vs_exploit(model, binary, variant=3, turns=200, seeds=(20260801, 4242)):
    """Mean land share for `model` against a scripted exploit, or None.

    Variant 3 is SCRIPT_BLITZ: never stops fighting, never makes peace, always
    attacks the weakest. Benched at difficulty 3, which is the rung that ships
    as the hardest and the one a player who wants a fight will pick.
    """
    vals = []
    env = dict(os.environ)
    env["OD_EVAL_MODEL"] = model
    for seed in seeds:
        try:
            out = subprocess.run(
                _gated([binary, "--eval-ai", "2", str(turns), str(seed), "3",
                 "--scenarios", "--vs-exploit", str(variant)]),
                cwd=ROOT, env=env, capture_output=True, text=True,
                errors="replace", timeout=3600).stdout
        except (subprocess.SubprocessError, OSError):
            return None
        got = None
        for line in out.splitlines():
            if line.startswith("[EVAL] land held"):
                try:
                    got = float(line.split("%")[0].split()[-1]) / 100.0
                except (ValueError, IndexError):
                    got = None
                break
        if got is None:
            return None
        vals.append(got)
    return sum(vals) / len(vals) if vals else None


def _keep_the_winner(shared, backup, binary):
    """Bench the merge against what the run STARTED from, and keep the better.

    ── WHY THIS EXISTS ──

    On 2026-08-28 an hour of training produced a merge that lost to its own
    starting point 0.63x head-to-head -- 0.69 and 0.56 on the two scenarios, so
    not noise -- and it was written straight over data/ai/model.bin. It was
    caught by hand. Nothing in the pipeline would have caught it, and nothing
    would have caught the next one either: training reports that the reward went
    up, which is a statement about the reward and not about whether the model
    plays better.

    A regression is not a failure of the run. Masks change, reward terms change,
    and a policy that has to re-equilibrate will get worse before it gets
    better. What must not happen is the worse one silently becoming the file the
    game loads.

    So: play the merge against the backup in both directions, average, and keep
    whichever actually wins. The loser is never deleted -- it is left beside the
    model as .rejected so the next run can start from it deliberately.
    """
    print("[POOL] benching the merge against the model this run started from...")
    fwd = _head_to_head(shared, backup, binary)
    rev = _head_to_head(backup, shared, binary)
    if fwd is None or rev is None:
        print("[POOL] bench did not produce a result; keeping the merge unchecked")
        return
    # Both readings are "share held by the first argument", so the merge's share
    # is the forward reading and one minus the reverse one.
    merged_share = (fwd + (1.0 - rev)) / 2.0
    print(f"[POOL] merge {merged_share*100:.1f}% of the land against its "
          f"predecessor (forward {fwd*100:.1f}%, reverse {(1.0-rev)*100:.1f}%)")
    # A 50/50 result is the expected one for a short run; only a real loss is
    # worth reverting for, or an unlucky wash costs an hour of genuine progress.
    if merged_share < 0.47:
        rejected = shared + ".rejected"
        shutil.copy2(shared, rejected)
        shutil.copy2(backup, shared)
        print(f"[POOL] REGRESSION: the merge lost to its own starting point. "
              f"Restored it; the merge is kept at {rejected}")
        return

    # ── ...AND AGAINST SOMETHING OUTSIDE ITS OWN LINEAGE ──
    #
    # The head-to-head above cannot see the failure that actually happened.
    # Measured on 2026-08-29: five hours of training produced a model that beat
    # its own predecessor -- every merge in between passed this guard -- while
    # losing SEVEN POINTS of land against a relentless rusher, on five worlds
    # out of five, band +/-2.2. A lineage can improve against itself forever
    # and still be getting worse at a strategy none of its members play.
    #
    # So the merge is also asked a question its ancestors cannot answer for it.
    # The threshold is deliberately loose: this is a backstop against the model
    # quietly forgetting how to survive a rush, not an opinion about how well it
    # ought to do -- it never beats the blitz outright, and a run that has to
    # re-equilibrate is allowed to wobble.
    before = _vs_exploit(backup, binary)
    after = _vs_exploit(shared, binary)
    if before is None or after is None:
        print("[POOL] blitz bench did not produce a result; keeping the merge")
        return
    print(f"[POOL] against a rusher: {after*100:.1f}% vs the predecessor's "
          f"{before*100:.1f}%")
    if after >= before - BLITZ_REGRESSION_MARGIN:
        print("[POOL] the merge holds up; keeping it")
        return
    rejected = shared + ".rejected"
    shutil.copy2(shared, rejected)
    shutil.copy2(backup, shared)
    print(f"[POOL] REGRESSION: the merge beat its predecessor head-to-head but "
          f"lost {(before-after)*100:.1f} points against a rusher. "
          f"Restored it; the merge is kept at {rejected}")
    return

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--max-rss-gb", type=float, default=MAX_WORKER_RSS_GB,
                    help="stop and restart a worker that exceeds this resident "
                         "size in GB (0 disables). See MAX_WORKER_RSS_GB")
    ap.add_argument("--workers", type=int, default=0,
                    help="worker processes (default: what memory allows)")
    ap.add_argument("--turns", type=int, default=10000, help="per-map turn cap")
    ap.add_argument("--limit", type=float, default=0,
                    help="total percent of the machine to use, split across workers")
    ap.add_argument("--seed", type=int, default=0, help="base seed (0 = clock)")
    ap.add_argument("--pbt", action="store_true",
                    help="population-based training: per-worker hyperparameters, "
                         "periodically exploited and perturbed")
    ap.add_argument("--binary", default=None,
                    help="exact worker binary to run, bypassing tree selection")
    ap.add_argument("extra", nargs="*", help="extra args passed to every worker")
    args = ap.parse_args()

    # Make Ctrl-C mean what the docstring says it means, even in the background.
    #
    # A shell starting this with `&` (nohup, a CI step, anything non-interactive)
    # sets SIGINT to SIG_IGN in the child, and Python KEEPS an inherited SIG_IGN
    # rather than installing its own handler -- so the interrupt path below was
    # simply unreachable for every backgrounded run, and the only way to stop one
    # was to kill the workers and let the wait loop fall through. Restoring the
    # default handler explicitly costs nothing when launched interactively and
    # makes "stop and merge" work when it was not.
    signal.signal(signal.SIGINT, signal.default_int_handler)

    # ...and make SIGTERM mean it too, which is what actually gets sent.
    #
    # Everything below -- stopping the workers, merging their models, keeping
    # the winner -- hangs off `except KeyboardInterrupt`, and SIGTERM does not
    # raise one. Python's default SIGTERM disposition kills the interpreter
    # outright, so `kill <pool-pid>` skipped the whole shutdown path: the
    # supervisor died, both workers were reparented to init and CARRIED ON
    # TRAINING into model.w*.bin that nothing would ever merge, and the shared
    # model was left at whatever the last periodic checkpoint happened to be.
    # That is the one failure mode that silently throws away a night of
    # training, and `kill` rather than Ctrl-C is how a backgrounded pool is
    # normally stopped.
    def _term(_sig, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, _term)

    binary = find_binary(args.binary)
    n = args.workers if args.workers > 0 else safe_worker_count()
    suggested = safe_worker_count()
    if args.workers > suggested:
        print(f"[POOL] warning: {args.workers} workers on a {machine_gb():.0f} GB machine; "
              f"{suggested} is what fits at ~{GB_PER_WORKER:.0f} GB each. "
              f"Expect swapping or OOM kills.")

    os.makedirs(DATA, exist_ok=True)
    shared = os.path.join(DATA, "model.bin")
    base_seed = args.seed or int(time.time())
    # The limit is a share of the WHOLE machine, so it has to be divided: four
    # workers each told "90%" would ask for 360% of it between them.
    per_worker_limit = (args.limit / n) if args.limit > 0 else 0

    procs = []
    worker_cmds = {}
    hypers = {}
    rng = random.Random(base_seed)
    for w in range(n):
        cmd = [binary]
        if per_worker_limit > 0:
            cmd += ["--resource-limit", f"{per_worker_limit:.2f}"]
        cmd += ["--train-ai", "0", str(args.turns), "0", str(base_seed + w * 7919)]
        cmd += ["--worker", str(w), "--workers", str(n)]
        cmd += args.extra
        worker_cmds[w] = list(cmd)
        print("[POOL] " + " ".join(cmd))
        env = os.environ.copy()
        if args.pbt:
            hypers[w] = _sample_hypers(rng, w)
            for k, v in hypers[w].items():
                env[k] = str(v)
            print(f"[PBT]  worker {w}: " +
                  " ".join(f"{k}={v}" for k, v in hypers[w].items()))
        procs.append(subprocess.Popen(_gated(cmd), cwd=ROOT, env=env))
        # Stagger: generating a world peaks well above its resident size, and
        # several workers generating at the same moment is the one point where a
        # pool that fits comfortably can still be killed for memory.
        time.sleep(20)

    print(f"[POOL] {n} worker(s) running. Ctrl-C to stop and merge.")
    if args.max_rss_gb > 0:
        print(f"[POOL] a worker over {args.max_rss_gb:.1f} GB resident is "
              f"restarted; see MAX_WORKER_RSS_GB")
    # A list so the wait loop below can rebind it without a global.
    last_bench = [time.time()]
    last_rss = [time.time()]
    try:
        t_start = time.time()
        next_pbt = t_start + PBT_INTERVAL
        if args.pbt:
            print(f"[PBT]  exploit/explore every {PBT_INTERVAL}s", flush=True)
        while any(p.poll() is None for p in procs):
            time.sleep(2)
            if not args.pbt or time.time() < next_pbt:
                continue
            next_pbt = time.time() + PBT_INTERVAL
            # SAY SO EVERY TIME, even when nothing is done. A round that
            # silently declines to act is indistinguishable from a loop that
            # never ran, which is exactly the hole the first PBT run fell into:
            # no output at all, and no way to tell whether the interval never
            # elapsed, the models were unreadable, or the ranking tied.
            print(f"[PBT]  round at {int(time.time() - t_start)}s", flush=True)
            # EXPLOIT then EXPLORE. Rank the workers by the fitness stored in
            # their own model file; the worst copies the best's WEIGHTS as well
            # as its hyperparameters, then jitters them. Copying the weights is
            # the part that makes this population-based rather than a random
            # sweep: a good setting is worth little without the progress it
            # made.
            scored = []
            for w in range(n):
                fit = _fitness(os.path.join(DATA, f"model.w{w}.bin"), binary,
                               seed=4242 + int(time.time() // PBT_INTERVAL))
                if fit is not None:
                    scored.append((fit, w))
            print(f"[PBT]  ranked {len(scored)}/{n} workers: " +
                  ", ".join(f"w{w}={f:+.4f}" for f, w in scored), flush=True)
            if len(scored) < 2:
                print("[PBT]  not enough readable models to rank; skipping round")
                continue
            scored.sort()
            worst_fit, worst = scored[0]
            best_fit, best = scored[-1]
            if worst == best:
                continue
            print(f"[PBT]  best w{best} ({best_fit:+.4f}) -> worst w{worst} "
                  f"({worst_fit:+.4f})")
            # A live worker is writing its model file; stop it before copying
            # over it, then relaunch with the inherited-and-jittered settings.
            if procs[worst].poll() is None:
                procs[worst].send_signal(signal.SIGTERM)
                deadline = time.time() + 30
                while procs[worst].poll() is None and time.time() < deadline:
                    time.sleep(0.5)
                if procs[worst].poll() is None:
                    procs[worst].kill()
            try:
                shutil.copy2(os.path.join(DATA, f"model.w{best}.bin"),
                             os.path.join(DATA, f"model.w{worst}.bin"))
            except OSError as e:
                print(f"[PBT]  copy failed ({e}); leaving worker {worst} as it was")
                continue
            hypers[worst] = _perturb(rng, hypers.get(best, {}))
            env = os.environ.copy()
            for k, v in hypers[worst].items():
                env[k] = str(v)
            print(f"[PBT]  worker {worst} restarts with " +
                  " ".join(f"{k}={v}" for k, v in hypers[worst].items()))
            procs[worst] = subprocess.Popen(_gated(worker_cmds[worst]), cwd=ROOT, env=env)

            # ── AND THE CEILING ──
            #
            # See MAX_WORKER_RSS_GB. Restarted rather than merely warned about:
            # a warning in a log nobody is reading is what let a machine reach
            # 97% swap and freeze. The worker's model is on disk, so this costs
            # a minute.
            if args.max_rss_gb > 0 and time.time() - last_rss[0] >= RSS_POLL_SECONDS:
                last_rss[0] = time.time()
                for w, pr in enumerate(procs):
                    if pr.poll() is not None:
                        continue
                    gb = worker_rss_gb(pr.pid)
                    if gb <= args.max_rss_gb:
                        continue
                    print(f"[POOL] worker {w} at {gb:.1f} GB is over the "
                          f"{args.max_rss_gb:.1f} GB ceiling; restarting it")
                    pr.send_signal(signal.SIGTERM)
                    deadline = time.time() + 60
                    while pr.poll() is None and time.time() < deadline:
                        time.sleep(0.5)
                    if pr.poll() is None:
                        pr.kill()
                    env = dict(os.environ)
                    for k, v in hypers[w].items():
                        env[k] = str(v)
                    procs[w] = subprocess.Popen(_gated(worker_cmds[w]), cwd=ROOT, env=env)

            # ── WHAT THE MODEL IS CURRENTLY WORTH, while it trains ──
            #
            # See BENCH_EVERY_SECONDS. Cheap, absolute, and written where the
            # training window can show it: the point is that a run drifting the
            # wrong way is visible AT THE TIME, not in a post-mortem. Failures
            # are swallowed -- an estimate that cannot be taken must never stop
            # a training run.
            if time.time() - last_bench[0] >= BENCH_EVERY_SECONDS:
                last_bench[0] = time.time()
                try:
                    out = subprocess.run(
                        [sys.executable, os.path.join(ROOT, "tools", "od_bench.py"),
                         "--quick", "--label", "live"],
                        cwd=ROOT, capture_output=True, text=True,
                        errors="replace", timeout=1800).stdout
                    for line in out.splitlines():
                        if "OD BENCH" in line:
                            print(f"[POOL] estimate:{line.split('OD BENCH')[1].strip()}")
                            break
                except (subprocess.SubprocessError, OSError):
                    pass
    except KeyboardInterrupt:
        print("\n[POOL] stopping workers...")
        for p in procs:
            if p.poll() is None:
                # SIGTERM, not SIGINT. The workers are raylib apps and install
                # no SIGINT handler that survives it -- signalled with SIGINT
                # they simply keep training, so Ctrl-C here waited out the full
                # 60s deadline and then killed them anyway, which is the one
                # outcome this path exists to avoid. The PBT branch above has
                # always used SIGTERM for the same reason. Measured: a worker
                # ignores SIGINT and exits on SIGTERM in about four seconds.
                p.send_signal(signal.SIGTERM)
        deadline = time.time() + 60
        for p in procs:
            while p.poll() is None and time.time() < deadline:
                time.sleep(0.5)
        for p in procs:
            if p.poll() is None:
                p.terminate()

    models = [os.path.join(DATA, f"model.w{w}.bin") for w in range(n)]
    models = [m for m in models if os.path.exists(m)]
    if not models:
        sys.exit("[POOL] no worker models to merge")

    backup = None
    if os.path.exists(shared):
        backup = shared + ".prev"
        shutil.copy2(shared, backup)
        print(f"[POOL] previous model kept at {backup}")

    print(f"[POOL] merging {len(models)} model(s) into {shared}")
    rc = subprocess.call([binary, "--merge-ai", shared] + models, cwd=ROOT)
    if rc == 0 and backup:
        _keep_the_winner(shared, backup, binary)
    sys.exit(rc)


if __name__ == "__main__":
    main()
