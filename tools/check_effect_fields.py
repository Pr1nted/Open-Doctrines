#!/usr/bin/env python3
"""
Does every effect a doctrine or a research node advertises actually DO anything?

    python3 tools/check_effect_fields.py            # report
    python3 tools/check_effect_fields.py --strict   # and fail on a dead one

WHY THIS EXISTS

An effect field is written in three places and read in one: a research node or
a doctrine sets it, Game::getResearchEffect sums it, and somewhere a resolver
is supposed to call getTotalEffect("thatName") and spend it. Miss the last step
and everything still compiles, the tooltip still prints the number, the doctrine
screen still advertises it -- and nothing happens. There is no symptom until a
player buys it and checks.

This has now happened at least four times:

  maintenanceCostPct  summed and never spent; four nodes and four doctrines
                      advertised an army upkeep cut that never applied
  indoctrinationPct   three nodes sold "minority alignment +5/10/20%/turn" and
                      getResearchEffect("indoctrinationPct") had no caller
  navyCostPct         reported by a player: "I researched Advanced
                      Shipbuilding, which says it reduces ship cost by 10%...
                      the cost was not reduced"
  passiveIncome, popModPct, resourceModPct  -- found by this tool

At the time it was written, 32 of 59 doctrines advertised at least one lever
that did nothing.

HOW IT DECIDES

The resolver names every field it can sum, in the if-chain in
Game::getResearchEffect. A field in that chain that no other file passes to
getTotalEffect or getResearchEffect is summed and never spent. That is the
whole check, and it is a text search -- it cannot tell you a resolver reads the
number and then ignores it, only that nobody reads it at all.
"""

import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESOLVER = os.path.join(ROOT, "src", "Game_Research.cpp")

# A field may be legitimately unread for a stated reason. Nothing is here yet,
# and anything added must say WHY in the comment beside it -- "not used" is not
# a reason, it is the fault this tool looks for.
ALLOWED_DEAD = {}


def fields_the_resolver_knows():
    with open(RESOLVER) as f:
        return sorted(set(re.findall(r'effectField == "(\w+)"', f.read())))


def readers(field):
    """Files that spend the field, excluding the resolver that sums it."""
    try:
        out = subprocess.run(
            ["grep", "-rn", f'getTotalEffect("{field}"', "--include=*.cpp",
             "--include=*.h", os.path.join(ROOT, "src")],
            capture_output=True, text=True).stdout
        out += subprocess.run(
            ["grep", "-rn", f'getResearchEffect("{field}"', "--include=*.cpp",
             "--include=*.h", os.path.join(ROOT, "src")],
            capture_output=True, text=True).stdout
    except OSError:
        return []
    return [l for l in out.splitlines() if "Game_Research.cpp" not in l]


def sellers(field):
    """How many research nodes and doctrines advertise it."""
    nodes = 0
    with open(RESOLVER) as f:
        nodes = len(re.findall(rf"\.{field}\s*=", f.read()))
    doctrines = []
    path = os.path.join(ROOT, "data", "policies.json")
    if os.path.exists(path):
        with open(path) as f:
            for p in json.load(f).get("policies", []):
                if field in (p.get("levers") or {}):
                    doctrines.append(p["id"])
    return nodes, doctrines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero if any advertised effect is never spent")
    args = ap.parse_args()

    dead, live = [], 0
    print(f"{'field':<22} {'nodes':>5} {'doctrines':>10}   readers")
    for field in fields_the_resolver_knows():
        r = readers(field)
        nodes, doctrines = sellers(field)
        mark = ""
        if not r:
            if field in ALLOWED_DEAD:
                mark = f"  (allowed: {ALLOWED_DEAD[field]})"
            else:
                dead.append((field, nodes, doctrines))
                mark = "   <-- NEVER SPENT"
        else:
            live += 1
        print(f"{field:<22} {nodes:>5} {len(doctrines):>10}   {len(r)}{mark}")

    print(f"\n{live} spent, {len(dead)} never spent, of "
          f"{len(fields_the_resolver_knows())} the resolver can sum")

    if dead:
        print("\nADVERTISED AND NEVER APPLIED:")
        for field, nodes, doctrines in dead:
            print(f"  {field}: {nodes} research node(s), {len(doctrines)} doctrine(s)")
            if doctrines:
                print(f"      {', '.join(doctrines[:6])}"
                      f"{' ...' if len(doctrines) > 6 else ''}")
        print("\nEach of these prints a number in a tooltip that nothing reads.")
        print("Either spend it in a resolver, or take it off the nodes that sell it.")

    return 1 if (dead and args.strict) else 0


if __name__ == "__main__":
    sys.exit(main())
