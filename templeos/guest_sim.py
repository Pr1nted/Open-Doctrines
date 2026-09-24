#!/usr/bin/env python3
"""A stand-in for the TempleOS client: reads turn.txt, writes orders.txt.

Two jobs.

**It is the reference client.** Whatever OpenDoc.HC ends up doing, it has to do
what this does: notice a turn it has not answered, parse the menus out of the
text, choose inside the budget, and write a line naming the turn it is for. This
is that contract in forty lines of Python, so the HolyC can be checked against
something executable rather than against prose.

**It is how the bridge is tested without TempleOS.** The bridge's own
--stub-player proves the engine side; this proves the part that matters more --
that a SEPARATE PROCESS, sharing nothing but a directory, can play a game. That
is exactly the guest's situation, minus the disk image.

    python3 templeos/guest_sim.py --dir /tmp/odbridge --turns 6
"""
from __future__ import annotations

import argparse
import os
import pathlib
import random
import re
import sys
import time

ITEM_RE = re.compile(r"([epwn]):(\d+) (.+?)(?=\s{2}[epwn]:\d|\s*$)")
MENU_RE = re.compile(r"^\[AGENT\] (economy|politics|war|navy)\s+(.*)$")
BUDGET_RE = re.compile(r"^\[AGENT\] budget e:(\d+) p:(\d+) w:(\d+) n:(\d+)")
HEAD_RE = re.compile(r"#\s*turn\s+(\d+)")

LETTER = {"economy": "e", "politics": "p", "war": "w", "navy": "n"}


def read_turn(path: pathlib.Path):
    """The turn on offer, or None if there is not a complete one to answer.

    Anything half-written reads as "not yet": the bridge renames turn.txt into
    place so this should not happen, but a guest reading a filesystem image the
    host is writing underneath it is exactly where it would.
    """
    try:
        text = path.read_text(encoding="ascii", errors="replace")
    except (FileNotFoundError, PermissionError):
        return None
    head, _, body = text.partition("\n")
    m = HEAD_RE.match(head.strip())
    if not m or "[AGENT] waiting" not in body:
        return None
    menus, budget = {}, {"e": 3, "p": 3, "w": 3, "n": 3}
    for line in body.splitlines():
        if mm := MENU_RE.match(line):
            menus[mm[1]] = [(int(n), name.strip()) for _, n, name in ITEM_RE.findall(mm[2])]
        elif bm := BUDGET_RE.match(line):
            budget = dict(zip("epwn", (int(x) for x in bm.groups())))
    return int(m[1]), menus, budget


def choose(menus, budget, rng) -> str:
    picks = []
    for key, letter in LETTER.items():
        items = menus.get(key, [])
        doing = [n for n, _ in items if n != 0]
        if not items:
            continue
        if not doing or rng.random() < 0.2:
            if any(n == 0 for n, _ in items):
                picks.append(f"{letter}:0")
            continue
        take = min(len(doing), budget.get(letter, 3), rng.randint(1, 2))
        picks += [f"{letter}:{n}" for n in rng.sample(doing, take)]
    return ", ".join(picks)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--turns", type=int, default=6, help="stop after this many")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--plant-stale", action="store_true",
                    help="before each real answer, drop an orders.txt for a turn "
                         "that is not on offer -- the bridge must ignore it")
    a = ap.parse_args()

    d = pathlib.Path(a.dir)
    turn_file, order_file = d / "turn.txt", d / "orders.txt"
    rng = random.Random(a.seed)
    answered: set[int] = set()
    deadline = time.time() + a.timeout

    while len(answered) < a.turns and time.time() < deadline:
        got = read_turn(turn_file)
        if got is None or got[0] in answered:
            time.sleep(0.05)
            continue
        turn_no, menus, budget = got

        # ── THE STALE FILE, PLANTED WHERE IT CAN ACTUALLY BE READ ──
        #
        # Planting one before the game starts proves nothing: the bridge clears
        # orders.txt when it starts, so it is gone before the first turn. It has
        # to land DURING a turn the bridge is waiting on. The pause is longer
        # than the bridge's poll so the file is guaranteed to be seen rather
        # than raced past -- a test that only sometimes exercises the thing it
        # is named after is worse than no test.
        if a.plant_stale:
            tmp = order_file.with_suffix(".part")
            tmp.write_text("# turn 9999\nSTALE:0\n", encoding="ascii")
            os.replace(tmp, order_file)
            time.sleep(0.4)

        tokens = choose(menus, budget, rng)
        # Named with its turn, and renamed into place: the bridge refuses a file
        # for a turn it is not on, which is the whole defence against replaying
        # a leftover answer.
        tmp = order_file.with_suffix(".part")
        tmp.write_text(f"# turn {turn_no}\n{tokens}\n", encoding="ascii")
        os.replace(tmp, order_file)
        answered.add(turn_no)
        if not a.quiet:
            print(f"guest: turn {turn_no} -> {tokens!r}")

    if len(answered) < a.turns:
        print(f"guest: only answered {len(answered)}/{a.turns} before the timeout",
              file=sys.stderr)
        return 1
    print(f"guest: answered {len(answered)} turn(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
