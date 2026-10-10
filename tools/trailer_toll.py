#!/usr/bin/env python3
"""What a war in this game actually cost, read out of a real save.

    python3 tools/trailer_toll.py "data/saves/Quick Start.odsv" --turns 120
    python3 tools/trailer_toll.py <save> --json docs/trailer/assets/toll.json

The trailer's AFTERMATH section puts a number on the screen, and a number on a
screen has to be true. This reads a .odsv's binary turn deltas and reports the
cost of conquest in it -- so the trailer quotes the game rather than a
copywriter.

WHAT THIS DELIBERATELY DOES NOT REPORT. There is no casualty counter in the
save format and there is no honest way to synthesise one:

  * Summing every province's turn-to-turn population DECLINE gives an enormous
    figure -- 661 billion over the 820-turn Quick Start, in a world of about
    six billion -- because it is dominated by MIGRATION. A person who leaves
    province A for province B is a loss in A and a gain in B, and counting only
    the losses counts the whole country's internal churn as deaths.
  * World population does not fall during these runs, it RISES: 2.05bn to
    3.60bn across 120 turns of Quick Start. So there is no net death signal to
    read at the world level either. (That growth is itself steeper than it
    should be, and over 820 turns it runs away completely -- see the open item
    on migration arithmetic in AGENTS.md. Do not quote long-horizon saves.)

So the figures below are the ones the data can support:

  conquests        ownership changes between two real countries (UNC/BLC
                   sentinels excluded -- colonising empty land is not conquest)
  people_flipped   population under those provinces at the moment they changed
                   hands.  FLIP-WEIGHTED: a province taken three times counts
                   its people three times, which is why `people_unique` is
                   reported beside it.  Say "flag changes", not "people".
  lost_after_conquest
                   population decline inside a province within six turns of it
                   being taken.  This is the closest thing in the data to a
                   cost of war, and it is the number the trailer quotes.
  emptied          provinces that fell below a quarter of their population

THE SAME RUN SUPPORTS THE OPPOSITE STORY, which is the point of the cut it
feeds: REBUILD quotes `world_pop_start` -> `world_pop_end` and `conquests` as
triumphs. Both framings are true of the same 120 turns. Nothing is invented in
either direction; the propaganda section does not lie, it selects.
"""
import argparse, json, os, sys, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import read_odsv                                               # noqa: E402

UNC, BLC = 65535, 65534      # unclaimed and blocked -- not countries
AFTER_WINDOW = 6             # turns a province counts as "just conquered"
EMPTIED_FRACTION = 0.25

def read_toll(path, cap=120):
    z = zipfile.ZipFile(path)
    dats = sorted(n for n in z.namelist() if n.endswith(".dat"))
    if cap:
        dats = dats[:cap]
    pop, owner, conquered_at = {}, {}, {}
    flips = people_flipped = lost_after = 0
    emptied, flipped_pids, unreadable = set(), set(), 0
    world = []
    for ti, n in enumerate(dats):
        try:
            t = read_odsv.unpack_turn(z.read(n))
        except Exception:
            unreadable += 1
            continue
        for pr in t["provinces"]:
            pid, ch = pr["province_id"], pr["changed"]
            if "population" in ch:
                new, old = ch["population"], pop.get(pid)
                if old is not None:
                    if (new < old and pid in conquered_at
                            and ti - conquered_at[pid] <= AFTER_WINDOW):
                        lost_after += old - new
                    if old > 1000 and new < old * EMPTIED_FRACTION:
                        emptied.add(pid)
                pop[pid] = new
            if "owner" in ch:
                o = ch["owner"]
                was = owner.get(pid)
                if was is not None and was != o \
                        and o not in (UNC, BLC) and was not in (UNC, BLC):
                    flips += 1
                    people_flipped += pop.get(pid, 0)
                    flipped_pids.add(pid)
                    conquered_at[pid] = ti
                owner[pid] = o
        world.append(sum(pop.values()))
    return {
        "save": os.path.basename(path),
        "turns": len(dats) - unreadable,
        "unreadable_turns": unreadable,
        "provinces": len(pop),
        "conquests": flips,
        "provinces_conquered": len(flipped_pids),
        "people_flipped": int(people_flipped),
        "people_unique": int(sum(pop.get(p, 0) for p in flipped_pids)),
        "lost_after_conquest": int(lost_after),
        "emptied": len(emptied),
        "world_pop_start": int(world[0]) if world else 0,
        "world_pop_end": int(world[-1]) if world else 0,
    }

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("save")
    ap.add_argument("--turns", type=int, default=120,
                    help="turn horizon (default 120; long saves run away)")
    ap.add_argument("--json", help="also write the figures here")
    a = ap.parse_args()
    t = read_toll(a.save, a.turns)
    w = max(len(k) for k in t)
    for k, v in t.items():
        print("%-*s  %s" % (w, k, "{:,}".format(v) if isinstance(v, int) else v))
    growth = (t["world_pop_end"] / t["world_pop_start"] - 1) * 100 \
        if t["world_pop_start"] else 0
    print("%-*s  %+.0f%%" % (w, "world_pop_growth", growth))
    if a.json:
        t["world_pop_growth_pct"] = round(growth, 1)
        os.makedirs(os.path.dirname(a.json) or ".", exist_ok=True)
        json.dump(t, open(a.json, "w"), indent=2)
        print("\nwrote", a.json)

if __name__ == "__main__":
    main()
