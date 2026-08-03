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
PBT_INTERVAL = 1800          # seconds between exploit/explore rounds
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


def _fitness(path):
    """Mean of the per-module running reward means, or None if unreadable.

    The reward-statistics blob is the LAST blob in the file: MOD_COUNT pairs of
    (mean, variance) floats. Parsed defensively -- a worker mid-save, or a model
    from a different build, must not take the whole pool down.
    """
    try:
        with open(path, "rb") as f:
            d = f.read()
        if len(d) < 6 or d[:4] != b"ODAI":
            return None
        p = 6
        blobs = []
        while p + 4 <= len(d):
            ln = int.from_bytes(d[p:p + 4], "little"); p += 4
            if p + ln > len(d):
                return None
            blobs.append(d[p:p + ln]); p += ln
        if not blobs:
            return None
        stats = blobs[-1]
        if len(stats) < 8 or len(stats) % 8 != 0:
            return None
        vals = struct.unpack("<" + "f" * (len(stats) // 4), stats)
        means = vals[0::2]
        return sum(means) / len(means) if means else None
    except OSError:
        return None


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data", "ai")

# One world measured at ~3 GB resident. Leave a few GB for the OS and whatever
# else the machine is doing, or the kernel starts killing workers mid-map and
# the run silently becomes a slower single-worker run.
GB_PER_WORKER = 3.0
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
        next_pbt = time.time() + PBT_INTERVAL
        while any(p.poll() is None for p in procs):
            time.sleep(2)
            if not args.pbt or time.time() < next_pbt:
                continue
            next_pbt = time.time() + PBT_INTERVAL
            # EXPLOIT then EXPLORE. Rank the workers by the fitness stored in
            # their own model file; the worst copies the best's WEIGHTS as well
            # as its hyperparameters, then jitters them. Copying the weights is
            # the part that makes this population-based rather than a random
            # sweep: a good setting is worth little without the progress it
            # made.
            scored = []
            for w in range(n):
                fit = _fitness(os.path.join(DATA, f"model.w{w}.bin"))
                if fit is not None:
                    scored.append((fit, w))
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
                p.send_signal(signal.SIGINT)
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
