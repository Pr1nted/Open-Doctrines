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
            [binary, "--resource-limit", "25", "--eval-ai", "1", "150", str(seed), "2",
             "--scenarios", "--vs-script"],
            cwd=ROOT, env=env, capture_output=True, text=True, timeout=600).stdout
    except (subprocess.SubprocessError, OSError):
        return None
    for line in out.splitlines():
        if line.startswith("[EVAL] land held"):
            try:
                return float(line.split("%")[0].split()[-1]) / 100.0
            except (ValueError, IndexError):
                return None
    return None


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data", "ai")

# Leave a few GB for the OS and whatever else the machine is doing, or the
# kernel starts killing workers mid-map and the run silently becomes a slower
# single-worker run.
#
# 0.7, not the 3.0 this used to say. A world WAS ~3 GB resident, and on a 16 GB
# machine that arithmetic allowed exactly three workers. Then the land/sea
# pixel buffer and four of the five resource buffers stopped being kept after
# load (LandSeaMap::dropPixels, Game_Loading.cpp), which took ~636 MB off every
# loaded world: three workers were measured at 0.4-0.6 GB resident each. The
# constant was never revisited, so the cap stayed where the old footprint put
# it and two thirds of the machine sat idle during training.
#
# Note this does NOT mean more workers is faster. --limit is a share of the
# MACHINE that gets divided among them (per_worker_limit below), so doubling
# the workers halves what each one gets and buys diversity, not throughput.
# Memory stopped being the binding constraint here; CPU budget still is.
GB_PER_WORKER = 0.7
GB_RESERVED = 4.0


def find_binary():
    for rel in (
        "build/OpenDoctrines.app/Contents/MacOS/OpenDoctrines",
        "build/OpenDoctrines",
        "build/OpenDoctrines.exe",
        "build-release/OpenDoctrines.app/Contents/MacOS/OpenDoctrines",
        "build-release/OpenDoctrines",
    ):
        p = os.path.join(ROOT, rel)
        if os.path.exists(p):
            return p
    sys.exit("No built binary found. Run: cmake --build build -j")


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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--workers", type=int, default=0,
                    help="worker processes (default: what memory allows)")
    ap.add_argument("--turns", type=int, default=10000, help="per-map turn cap")
    ap.add_argument("--limit", type=float, default=0,
                    help="total percent of the machine to use, split across workers")
    ap.add_argument("--seed", type=int, default=0, help="base seed (0 = clock)")
    ap.add_argument("--pbt", action="store_true",
                    help="population-based training: per-worker hyperparameters, "
                         "periodically exploited and perturbed")
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

    binary = find_binary()
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
        procs.append(subprocess.Popen(cmd, cwd=ROOT, env=env))
        # Stagger: generating a world peaks well above its resident size, and
        # several workers generating at the same moment is the one point where a
        # pool that fits comfortably can still be killed for memory.
        time.sleep(20)

    print(f"[POOL] {n} worker(s) running. Ctrl-C to stop and merge.")
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
            procs[worst] = subprocess.Popen(worker_cmds[worst], cwd=ROOT, env=env)
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

    if os.path.exists(shared):
        backup = shared + ".prev"
        shutil.copy2(shared, backup)
        print(f"[POOL] previous model kept at {backup}")

    print(f"[POOL] merging {len(models)} model(s) into {shared}")
    rc = subprocess.call([binary, "--merge-ai", shared] + models, cwd=ROOT)
    sys.exit(rc)


if __name__ == "__main__":
    main()
