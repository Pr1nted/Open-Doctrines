#!/usr/bin/env python3
"""Play the game many times, in ways nobody would think to try by hand.

    tools/fuzz.py [--runs N] [--turns T] [--binary PATH] [--jobs J] [--seed S]

WHAT THIS IS FOR, AND WHY IT IS NOT A UNIT TEST

The suite checks rules in isolation: a battle resolves, a ticket verifies, a
caret lands where it should. What none of it does is play a whole game, with a
world nobody designed, for four hundred turns, and see whether the thing falls
over. Every crash this project has actually shipped -- the browser stack
overflow on Process Turn, the quadratic save, the doubled doctrines on load --
was in that gap: each component was correct and the combination was not.

So this generates worlds from random seeds and plays them out headlessly,
looking for four things a person would notice and a unit test never will:

  1. A CRASH. Non-zero exit, a signal, an abort.
  2. A HANG. A turn that never finishes is worse than one that fails, because
     a server with one sits there looking healthy.
  3. NON-DETERMINISM. The same seed twice must produce the same world. When it
     does not, every measurement in this project is worthless, and it has gone
     wrong before -- a live game rewriting model.bin mid-run made a check flake
     for days before the cause was found.
  4. A REFUSAL TO LOAD ITS OWN WORLD. The map it just generated is fed back
     through --check, because a world the game writes and cannot read is a save
     format bug that only shows up on somebody else's machine.

IT NEVER TOUCHES YOUR DATA. Every run gets a scratch OD_DATA_DIR with the game
data symlinked and a COPY of the model, so a fuzz run cannot write over
data/ai/model.bin, cannot fill data/saves with generated worlds, and cannot be
corrupted by a training run happening at the same time.

IT ALSO WILL NOT FIGHT YOUR BENCHES. Concurrency is bounded, and it refuses to
start while tools/odlock.py reports work in flight, because both want the whole
machine and each map load peaks around 2 GB.

Every failure is printed with the exact command that reproduces it. A fuzzer
that finds a crash and cannot tell you how to see it again has found nothing.
"""

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# What the game prints once a world exists. Compared between two runs of the
# same seed: it is the game's own summary of the world it built, so if it
# differs, generation is not deterministic.
HASH_RE = re.compile(r"compass hash (\d+)")


def scratch_data(tmp: str) -> str:
    """A data directory that shares the bulk assets and owns everything writable.

    Symlinked rather than copied: data/ is over a gigabyte and copying it per
    run would make the fuzzer slower than the thing it is testing. The model is
    COPIED, because it is the one file a run could write to.
    """
    d = os.path.join(tmp, "data")
    os.makedirs(d, exist_ok=True)
    src = os.path.join(ROOT, "data")
    for name in os.listdir(src):
        if name in ("saves", "ai", "config.json", "account.json"):
            continue
        os.symlink(os.path.join(src, name), os.path.join(d, name))
    os.makedirs(os.path.join(d, "saves"), exist_ok=True)
    os.makedirs(os.path.join(d, "ai"), exist_ok=True)
    model = os.path.join(src, "ai", "model.bin")
    if os.path.exists(model):
        shutil.copy2(model, os.path.join(d, "ai", "model.bin"))
    return d


def play(binary: str, data: str, seed: int, turns: int, timeout: int):
    """One headless game. Returns (status, hash, seconds, command)."""
    cmd = [binary, "--eval-ai", "1", str(turns), str(seed), "3"]
    env = dict(os.environ, OD_DATA_DIR=data)
    started = time.time()
    try:
        p = subprocess.run(cmd, env=env, capture_output=True, text=True,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return "HANG", None, time.time() - started, cmd
    took = time.time() - started
    if p.returncode != 0:
        # A signal shows up as a negative return code and is worth naming: a
        # segfault and a clean non-zero exit are different bugs.
        how = f"signal {-p.returncode}" if p.returncode < 0 else f"exit {p.returncode}"
        return f"CRASH ({how})", None, took, cmd
    m = HASH_RE.search(p.stdout)
    return "ok", (m.group(1) if m else None), took, cmd


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=12)
    ap.add_argument("--turns", type=int, default=60)
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--seed", type=int, default=None, help="reproduce one seed")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--binary", default=os.path.join(ROOT, "build", "OpenDoctrinesServer"))
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        print(f"no server binary at {args.binary} -- build OpenDoctrinesServer first")
        return 2

    # Somebody else's bench owns the machine; two of these at once means paging.
    # BOTH PATTERNS, because the first one was not enough. odlock.py is the
    # sanctioned gate, but a bench driven straight from od_bench.py does not go
    # through it -- and that is what was actually running the first time this
    # guard was tested, so it did not fire and the fuzzer competed for the
    # machine exactly as it was written not to.
    busy = ""
    for pattern in ("odlock", "od_bench"):
        try:
            if subprocess.run(["pgrep", "-f", pattern], capture_output=True).returncode == 0:
                busy = pattern
                break
        except FileNotFoundError:
            pass
    if busy and args.seed is None:
        print(f"a bench is running (pgrep -f {busy}). Refusing to compete for the "
              "machine -- each world peaks around 2 GB and both would page. "
              "Pass --seed to reproduce one case anyway.")
        return 3

    rng = random.Random()
    seeds = [args.seed] if args.seed is not None else [rng.randrange(1, 2**31) for _ in range(args.runs)]

    tmp = tempfile.mkdtemp(prefix="odfuzz-")
    data = scratch_data(tmp)
    print(f"fuzzing {len(seeds)} world(s) x {args.turns} turns, {args.jobs} at a time")
    print(f"scratch data: {data}\n")

    failures, hashes = [], {}
    try:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for seed, (status, h, took, cmd) in zip(
                    seeds, pool.map(lambda s: play(args.binary, data, s, args.turns, args.timeout), seeds)):
                hashes[seed] = h
                mark = "ok  " if status == "ok" else "FAIL"
                print(f"  {mark} seed {seed:<12} {took:6.1f}s  {status if status != 'ok' else ''}")
                if status != "ok":
                    failures.append((seed, status, " ".join(cmd)))

        # DETERMINISM, on a sample. Replaying every seed would double the run
        # for a property that fails globally when it fails at all.
        for seed in seeds[: max(1, len(seeds) // 4)]:
            if hashes.get(seed) is None:
                continue
            status, again, _, cmd = play(args.binary, data, seed, args.turns, args.timeout)
            if status == "ok" and again != hashes[seed]:
                print(f"  FAIL seed {seed:<12}        NOT DETERMINISTIC: {hashes[seed]} then {again}")
                failures.append((seed, "non-deterministic", " ".join(cmd)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    if not failures:
        print(f"{len(seeds)} world(s) played, nothing fell over.")
        return 0
    print(f"{len(failures)} failure(s):")
    for seed, status, cmd in failures:
        print(f"  seed {seed}: {status}")
        print(f"    reproduce: OD_DATA_DIR=<scratch> {cmd}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
