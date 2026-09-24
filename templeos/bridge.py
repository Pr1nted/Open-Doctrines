#!/usr/bin/env python3
"""The host half of the TempleOS bridge: a game of Open Doctrines, as two files.

TempleOS has no networking, so the FILE is the network. This runs the engine's
agent door on the host and exchanges two plain files with whatever is playing:

    <dir>/turn.txt     written by this; one turn, exactly as the door printed it
    <dir>/orders.txt   written by the player; one line of `module:action` tokens

and repeats until the game ends. Nothing here knows or cares how that directory
reaches a guest -- a QEMU vvfat drive, a FAT image swapped between turns, or a
person copying files by hand. That is deliberate: the transport is the one part
that needs a running TempleOS to test, so it is kept outside the part that does
not.

    python3 templeos/bridge.py --build build --dir /tmp/odbridge
    python3 templeos/bridge.py --build build --dir /tmp/odbridge --stub-player

--stub-player plays the game itself with a trivial policy. That is how the whole
host half is tested without TempleOS anywhere near it.

── THE STALE-ORDER PROBLEM, WHICH IS THE ONE THAT BITES ──

A file left over from turn 4 looks exactly like a file written for turn 5. If
the bridge read whatever was lying there it would replay the old line and the
player would never be asked. So orders.txt must NAME ITS TURN:

    # turn 5
    e:1, w:1

and anything whose turn does not match the turn on offer is ignored and waited
out. turn.txt carries the same header, so a client can tell a fresh turn from
one it has already answered -- which it must, because a file's timestamp does
not survive most ways of getting it across.
"""
from __future__ import annotations

import argparse
import errno
import os
import pathlib
import random
import re
import subprocess
import sys
import threading
import time

TURN_RE = re.compile(r"^\[AGENT\] ===== turn (\d+)/(\d+)")
MENU_RE = re.compile(r"^\[AGENT\] (economy|politics|war|navy)\s+(.*)$")
ITEM_RE = re.compile(r"([epwn]):(\d+) (.+?)(?=\s{2}[epwn]:\d|\s*$)")
BUDGET_RE = re.compile(r"^\[AGENT\] budget e:(\d+) p:(\d+) w:(\d+) n:(\d+)")


def write_atomic(path: pathlib.Path, text: str) -> None:
    """Write so a reader never sees half a file.

    It matters more here than usual: the reader may be a guest OS looking at a
    filesystem image the host is writing underneath it, and a torn turn.txt
    would read as a protocol error rather than as a timing one.
    """
    tmp = path.with_suffix(path.suffix + ".part")
    tmp.write_text(text, encoding="ascii", errors="replace")
    os.replace(tmp, path)


def send_line(fifo: str, line: str, proc: subprocess.Popen, timeout: float = 60.0) -> None:
    """One line into the door's FIFO, without ever hanging on a dead engine.

    The door prints `waiting` BEFORE it opens the FIFO for reading, so the first
    attempt can find no reader at all (ENXIO). Retry until it appears, and give
    up if the process is gone -- the same dance Open Fly's driver does, for the
    same reason.
    """
    deadline = time.time() + timeout
    while True:
        try:
            fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
            break
        except OSError as e:
            if e.errno != errno.ENXIO:
                raise
            if proc.poll() is not None:
                raise RuntimeError("the engine exited before reading the turn")
            if time.time() > deadline:
                raise TimeoutError("the engine never opened the FIFO for reading")
            time.sleep(0.01)
    try:
        os.write(fd, (line + "\n").encode("ascii", "replace"))
    finally:
        os.close(fd)


def parse_menus(block: list[str]) -> dict[str, list[tuple[int, str]]]:
    out: dict[str, list[tuple[int, str]]] = {}
    for line in block:
        if m := MENU_RE.match(line):
            out[m[1]] = [(int(n), name.strip()) for _, n, name in ITEM_RE.findall(m[2])]
    return out


def parse_budget(block: list[str]) -> dict[str, int]:
    for line in block:
        if m := BUDGET_RE.match(line):
            return dict(zip("epwn", (int(x) for x in m.groups())))
    return {"e": 3, "p": 3, "w": 3, "n": 3}


class StubPlayer:
    """A policy that stands in for a person at a TempleOS keyboard.

    Not trying to play well -- trying to produce the same SHAPE of answer a
    person would, so the bridge is exercised: a couple of real actions per
    module, inside the budget, sometimes a plain hold. Seeded, so a failing run
    can be repeated.
    """

    def __init__(self, seed: int = 0) -> None:
        self.rng = random.Random(seed)

    def __call__(self, menus, budget) -> str:
        picks: list[str] = []
        for letter, key in (("e", "economy"), ("p", "politics"), ("w", "war"), ("n", "navy")):
            items = menus.get(key, [])
            if not items:
                continue
            doing = [n for n, _ in items if n != 0]
            if not doing or self.rng.random() < 0.25:
                if any(n == 0 for n, _ in items):
                    picks.append(f"{letter}:0")
                continue
            take = min(len(doing), budget.get(letter, 3), self.rng.randint(1, 2))
            picks += [f"{letter}:{n}" for n in self.rng.sample(doing, take)]
        return ", ".join(picks)


def run(build: str, bridge_dir: str, seat: str, seed: int, turns: int,
        data: str, stub, poll: float, quiet: bool) -> int:
    d = pathlib.Path(bridge_dir)
    d.mkdir(parents=True, exist_ok=True)
    turn_file, order_file = d / "turn.txt", d / "orders.txt"
    # A leftover from a previous game is exactly the stale file this protocol
    # exists to refuse, so it goes before the first turn rather than being
    # relied on to mismatch.
    order_file.unlink(missing_ok=True)

    fifo = str(d / "agent.fifo")
    if os.path.exists(fifo):
        os.unlink(fifo)
    os.mkfifo(fifo)

    server = str(pathlib.Path(build) / "OpenDoctrinesServer")
    if not os.access(server, os.X_OK):
        print(f"no {server} -- build the OpenDoctrinesServer target first", file=sys.stderr)
        return 1

    env = dict(os.environ, OD_WORLD_SEED=str(seed))
    proc = subprocess.Popen(
        [server, "--bench-agent", seat, fifo, "--until", str(turns),
         "--seed", str(seed), "--data", data],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1, env=env)

    lines: list[str] = []
    done = threading.Event()

    def pump():
        # Drained continuously: a full stdout pipe blocks the engine mid-turn.
        for ln in proc.stdout:
            lines.append(ln.rstrip("\n"))
        done.set()

    threading.Thread(target=pump, daemon=True).start()

    seen, played, at = 0, 0, 0
    try:
        while not done.is_set() or at < len(lines):
            if at >= len(lines):
                time.sleep(0.02)
                continue
            line = lines[at]
            at += 1
            if line != "[AGENT] waiting":
                continue

            block = lines[:at]
            turn_no = 0
            for ln in reversed(block):
                if m := TURN_RE.match(ln):
                    turn_no = int(m[1])
                    break
            body = "\n".join(l for l in block[seen:] if l.startswith("[AGENT]"))
            seen = at
            write_atomic(turn_file, f"# turn {turn_no}\n{body}\n")

            menus, budget = parse_menus(block), parse_budget(block)
            if stub is not None:
                tokens = stub(menus, budget)
                write_atomic(order_file, f"# turn {turn_no}\n{tokens}\n")

            tokens = wait_for_orders(order_file, turn_no, proc, poll)
            if tokens is None:
                print("the engine exited while waiting for orders", file=sys.stderr)
                break
            order_file.unlink(missing_ok=True)
            send_line(fifo, tokens, proc)
            played += 1
            if not quiet:
                print(f"turn {turn_no}: sent {tokens!r}")

        proc.wait(timeout=120)
    finally:
        if proc.poll() is None:
            proc.kill()
        if os.path.exists(fifo):
            os.unlink(fifo)

    score = next((l for l in lines if l.startswith("[BENCH]")), "")
    print(f"bridge: {played} turn(s) exchanged. {score}")
    return 0 if played else 1


def wait_for_orders(path: pathlib.Path, turn_no: int, proc, poll: float):
    """Block until an orders file for THIS turn appears.

    A file for another turn is left alone rather than deleted: it may be one the
    player is still writing, and deleting somebody's in-progress answer to tell
    them it is stale is not an improvement.
    """
    while True:
        if proc.poll() is not None:
            return None
        try:
            text = path.read_text(encoding="ascii", errors="replace")
        except (FileNotFoundError, PermissionError):
            time.sleep(poll)
            continue
        head, _, rest = text.partition("\n")
        m = re.match(r"#\s*turn\s+(\d+)", head.strip())
        if not m:
            # No header at all: an older client, or somebody typing by hand.
            # Accept it, because refusing a turn over a missing comment would be
            # pedantry, and say so once.
            body = text.strip()
            if body:
                return body
            time.sleep(poll)
            continue
        if int(m[1]) != turn_no:
            time.sleep(poll)
            continue
        return rest.strip()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default="build")
    ap.add_argument("--dir", required=True, help="the exchange directory")
    ap.add_argument("--seat", default="1914:SWE:rung")
    ap.add_argument("--seed", type=int, default=20260801)
    ap.add_argument("--turns", type=int, default=120)
    ap.add_argument("--data", default="data")
    ap.add_argument("--poll", type=float, default=0.25,
                    help="seconds between looks for orders.txt")
    ap.add_argument("--stub-player", action="store_true",
                    help="play it here instead of waiting for a guest")
    ap.add_argument("--stub-seed", type=int, default=1)
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    stub = StubPlayer(a.stub_seed) if a.stub_player else None
    return run(a.build, a.dir, a.seat, a.seed, a.turns, a.data, stub, a.poll, a.quiet)


if __name__ == "__main__":
    raise SystemExit(main())
