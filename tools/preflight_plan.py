#!/usr/bin/env python3
"""Work out the fewest multiplayer sessions that prove cross-play, and fit them in RAM.

THE PROBLEM

Every platform must be shown to HOST a game and to JOIN one, and every pair of
platforms must be shown to actually play together. Done naively that is every
ordered (host, client) pair: with six platforms, thirty sessions, each needing
a VM booted and a game driven. That is hours per run and nobody would run it.

Three observations collapse it.

  1. A SESSION HAS MANY CLIENTS. One session of k participants proves (k-1)
     host-to-client pairs at once, and every pair of CLIENTS in it plays
     together too -- their turns pass through the host and land in each other's
     worlds. A session of four proves six pairs, not one.

  2. HOSTING AND JOINING ARE DIFFERENT CODE PATHS, and that is the whole
     reduction. What has to be true is that each platform can host and each can
     join -- not that every ordered combination has been tried. So each
     platform hosts exactly ONE session. Six sessions, not thirty.

  3. THE HOST MACHINE IS FREE. The Mac this runs on is already up, and a native
     client costs it a fraction of a VM. It can sit in every session without
     spending any of the budget, which is what makes k=4 reachable at all.

What remains is a covering problem: choose the CLIENTS of each session so that
every unordered pair co-occurs somewhere, subject to the memory each guest
costs. Six blocks of four cover thirty-six pair-slots against fifteen pairs, so
coverage is comfortable once every platform has hosted -- the scheduler checks
it rather than assuming it.

    python3 tools/preflight_plan.py
    python3 tools/preflight_plan.py --budget-gb 24 --json plan.json
    python3 tools/preflight_plan.py --only mac,linux,windows
"""
import argparse
import itertools
import json
import sys

# ── the platforms, and what each costs to have running ──
#
# "cost" is the RAM a participant needs while a session is live: a VM's
# configured memory, an emulator's heap, or ~0 for a native client on the
# machine that is already on. Measured from tools/*_vm_create.sh defaults; the
# Android figure is an AVD with 2 GB plus its own overhead.
PLATFORMS = {
    # name        cost_gb  how it participates
    "mac":       (0.0, "native on the host machine"),
    "linux":     (4.0, "UTM: OD-debian12-arm64"),
    "windows":   (6.0, "UTM: Windows11"),
    "freebsd":   (2.0, "UTM: OD-freebsd14-arm64"),
    "openbsd":   (2.0, "UTM: OD-openbsd76-arm64"),
    "android":   (3.0, "emulator: AVD od-test"),
}
# The host machine is always up, so it never counts against the budget and can
# join every session. Everything else has to be booted and torn down.
ALWAYS_UP = "mac"


def sessions_for(names, budget_gb):
    """One session per platform-as-host, clients chosen to cover every pair.

    Greedy, and deliberately so: the optimum here differs from the greedy
    answer by at most a session or two on six platforms, and a schedule a
    person can read and predict is worth more than one that is provably
    minimal. The coverage check at the end is what has to be right.
    """
    cost = {n: PLATFORMS[n][0] for n in names}
    need = {frozenset(p) for p in itertools.combinations(sorted(names), 2)}
    covered = set()
    plan = []

    # Every platform hosts exactly once. Most expensive first: a session hosted
    # by Windows has the least room left for clients, so it should pick its
    # clients while the uncovered pairs are still plentiful.
    for host in sorted(names, key=lambda n: -cost[n]):
        room = budget_gb - (0.0 if host == ALWAYS_UP else cost[host])
        clients = []
        # The host machine joins free, and does so first -- it is the one
        # participant that never costs anything.
        if ALWAYS_UP in names and host != ALWAYS_UP:
            clients.append(ALWAYS_UP)
        # Then whichever remaining platform closes the most uncovered pairs per
        # gigabyte. Ties break by name so two runs produce the same plan.
        while True:
            best, best_score = None, 0.0
            for c in sorted(names):
                if c == host or c in clients:
                    continue
                if cost[c] > room:
                    continue
                gain = sum(1 for other in clients + [host]
                           if frozenset((c, other)) not in covered)
                if gain == 0:
                    continue
                score = gain / max(cost[c], 0.25)
                if score > best_score:
                    best, best_score = c, score
            if best is None:
                break
            clients.append(best)
            room -= cost[best]

        # A HOST WITH NO CLIENTS PROVES NOTHING. The greedy above stops when no
        # remaining platform closes an uncovered pair, which on the last host is
        # every time -- leaving a "session" of one machine talking to itself and
        # a hosting capability still untested. Take the cheapest thing that
        # fits instead.
        if not clients:
            afford = [c for c in sorted(names)
                      if c != host and cost[c] <= room]
            if afford:
                clients.append(min(afford, key=lambda c: (cost[c], c)))
                room -= cost[clients[-1]]

        for a, b in itertools.combinations(clients + [host], 2):
            covered.add(frozenset((a, b)))
        used = sum(cost[c] for c in clients if c != ALWAYS_UP)
        used += 0.0 if host == ALWAYS_UP else cost[host]
        plan.append({"host": host, "clients": clients, "gb": round(used, 1)})

    # ── the expensive pairs the greedy strands ──
    #
    # Scoring by pairs-per-gigabyte is right while there is plenty to cover,
    # and wrong at the end: the two costliest platforms (Windows at 6 GB and
    # Linux at 4) are never the best value, so they never meet, and
    # (linux, windows) came out untested -- the pair most likely to matter,
    # because it is the two platforms most players are on. So: repair.
    #
    # Each repair session is one uncovered pair, hosted by the cheaper of the
    # two so the dearer one is a client. Extra sessions, deliberately: a pair
    # that will not fit beside anything else still has to be proven somewhere.
    for pair in sorted(need - covered, key=lambda p: sorted(p)):
        a, b = sorted(pair, key=lambda n: (cost[n], n))
        if cost[a] + cost[b] > budget_gb:
            continue                      # genuinely does not fit; reported below
        plan.append({"host": a, "clients": [b],
                     "gb": round((0.0 if a == ALWAYS_UP else cost[a]) + cost[b], 1),
                     "repair": True})
        covered.add(frozenset((a, b)))

    missing = need - covered
    return plan, missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--budget-gb", type=float, default=11.0,
                    help="RAM available for guests. Default 11: a 16 GB Mac "
                         "with the desktop, the browser and the build tree "
                         "already in it.")
    ap.add_argument("--only", help="comma-separated subset of platforms")
    ap.add_argument("--json", help="write the plan here for the runner")
    a = ap.parse_args()

    names = sorted(PLATFORMS)
    if a.only:
        names = [n.strip() for n in a.only.split(",") if n.strip()]
        bad = [n for n in names if n not in PLATFORMS]
        if bad:
            sys.exit(f"unknown platform(s): {', '.join(bad)}")
    if len(names) < 2:
        sys.exit("cross-play needs at least two platforms")

    plan, missing = sessions_for(names, a.budget_gb)

    pairs = len(names) * (len(names) - 1) // 2
    naive = len(names) * (len(names) - 1)
    print(f"{len(names)} platforms, {a.budget_gb:g} GB for guests")
    print(f"  every ordered host/client pair would be {naive} sessions")
    print(f"  every platform hosting once is {len(plan)}")
    print()
    print(f"{'#':<3}{'host':<10}{'clients':<34}{'GB':>5}  covers")
    for i, s in enumerate(plan, 1):
        cov = ", ".join(f"{s['host']}>{c}" for c in s["clients"])
        print(f"{i:<3}{s['host']:<10}{', '.join(s['clients']):<34}{s['gb']:>5}  {cov}")
    print()
    print(f"pairs that must play together: {pairs}")
    if missing:
        print(f"  NOT COVERED: {sorted(tuple(sorted(m)) for m in missing)}")
        print("  raise --budget-gb, or accept that those pairs are untested")
    else:
        print("  all covered")
    over = [s for s in plan if s["gb"] > a.budget_gb + 1e-9]
    if over:
        print(f"  OVER BUDGET: {[s['host'] for s in over]}")

    if a.json:
        with open(a.json, "w") as f:
            json.dump({"budget_gb": a.budget_gb, "platforms": names,
                       "sessions": plan,
                       "uncovered": sorted(sorted(m) for m in missing)}, f, indent=2)
        print(f"\nwrote {a.json}")
    return 1 if missing or over else 0


if __name__ == "__main__":
    sys.exit(main())
