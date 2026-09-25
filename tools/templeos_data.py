#!/usr/bin/env python3
"""Bake the tables the TempleOS build needs: research, policies, claims.

Three sources, three shapes, one file:

  * RESEARCH comes from C++ SOURCE. buildResearchNodes() in Game_Research.cpp
    is eighty-odd add() calls, not a data file, so this parses them. That is
    deliberate rather than lazy: the desktop game's tree stays the one
    definition, and tools/templeos_sync.py fails the build when the count here
    stops matching it.
  * POLICIES come from policies.json inside the .odmap.
  * CLAIMS come from claims.json, which is {ISO: [province ids]}.

Claims are kept as province IDS, not indices. The world file's indices depend
on the raster resolution it was baked at, and two files that must be baked at
the same resolution to agree is exactly the sort of coupling that breaks
quietly six months later. The guest maps ids to indices on load.

    python3 tools/templeos_data.py data/STDmaps/map.odmap templeos/game.odd
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import struct
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
RESEARCH_SRC = ROOT / "src" / "Game_Research.cpp"

MAGIC, VERSION = b"ODTD", 3
MAX_DEPS = 4

# The five categories the research tree uses, as fixed slots so the guest can
# group without carrying strings it would only compare.
CATS = ["buildings", "army", "formations", "population", "efficiency", "misc"]

# ── THE LEVERS THIS BUILD HONOURS ──
#
# A policy in the desktop game pulls up to twenty different levers. The
# TempleOS rules are income, recruitment, combat and population, so these
# eight are the ones with something to act on -- and each is applied, not
# merely displayed.
#
# Everything else is listed below it, with the reason. That list is not
# decoration: templeos_sync.py fails the build when policies.json grows a
# lever that appears in neither, so a new effect cannot be silently ignored
# by a build that still charges you for the policy.
LEVERS = [
    "popGrowthPct",         # population growth, per turn
    "passiveIncome",        # flat money per turn
    "resourceModPct",       # scales province income
    "armyAtkPct",           # assault strength
    "armyDefPct",           # defence strength
    "conscriptionPct",      # how much of a population can be raised
    "conscriptionCostPct",  # what raising it costs
    "maintenanceCostPct",   # what keeping it costs
    "navyAtkPct",           # bombardment
    "navyDefPct",           # a fleet defending its harbour
    "navyCostPct",          # what a hull costs to lay down
    "navySpeedPct",         # how far a fleet sails in a turn
]
LEVER_SCALE = 10            # stored as tenths, so 0.5 survives as an integer

# ── ARTILLERY ──
#
# The eight shell types, their effects and their prices, transcribed from the
# desktop game rather than invented: ALL_ARTY in Game_Render.cpp for what each
# one does, artyCosts beside it for what it costs, and getNodeId for the
# research that unlocks it. Numbers this specific are worth copying exactly --
# a Nuclear shell that kills 70% instead of 75% is a different game quietly.
ARTY = [
    # name,          node,     troopKill, popKill, fortDmg, indDmg, fortChance, cost
    ("Mortar",       "arty1",   5,  0, 0, 0,  0,  5),
    ("Light Arty",   "arty2",  10,  0, 0, 0,  0, 10),
    ("Heavy Arty",   "arty3",  20,  5, 0, 0,  0, 20),
    ("Napalm",       "arty4a", 25, 15, 0, 0,  0, 30),
    ("Carpet Bomb",  "arty4b", 15, 10, 0, 0, 50, 25),
    ("Chemical",     "arty5",  50, 30, 0, 0,  0, 40),
    ("Nuclear",      "arty6a", 75,  0, 2, 3,  0, 80),
    ("Biological",   "arty6b", 80, 95, 0, 0,  0, 60),
]

LEVERS_SKIPPED = {
    "indoctrinationPct":   "no minority or alignment model",
    "industryCostPct":     "industry cannot be built here",
    "industryUpkeepPct":   "industry cannot be built here",
    "migrationRate":       "no migration model",
    "popModPct":           "no per-province population modifier",
    "specSubsidyRoomPct":  "no industry specialisation",
    "specTaxRoomPct":      "no industry specialisation",
    "warDeclarations":     "one war at a time, by construction",
}


def fixed(s: str, n: int) -> bytes:
    return s.encode("ascii", "replace")[: n - 1].ljust(n, b"\0")


def parse_research(src: str):
    """Every add(...) call in buildResearchNodes, in order.

    Matched on the call's first six arguments, which is all that is needed and
    all that can be relied on: several nodes chain a `.fortLevel=` or similar
    onto the end and some span three lines.
    """
    body = src[src.index("void buildResearchNodes"):]

    # ── COMMENTS OUT FIRST ──
    #
    # Arguments are separated by whitespace in the happy case and by a
    # paragraph of explanation in at least one: assault_doctrine carries a
    # comment between its description and its category, and a pattern that
    # only tolerates whitespace there silently drops it. Eighty-five nodes
    # where the source has eighty-six is exactly the kind of near-miss nobody
    # notices.
    body = re.sub(r"//[^\n]*", "", body)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)

    nodes = []
    pat = re.compile(
        r'add\(\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"[^"]*"\s*,\s*'
        r'"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*\{([^}]*)\}\s*,\s*(\d+)', re.S)
    for m in pat.finditer(body):
        deps = re.findall(r'"([^"]+)"', m[5])
        # The SUBcategory matters as much as the category: the naval ladder is
        # category "army", subcategory "navy", so "focus navy" finds nothing
        # if only the category is carried across.
        nodes.append({"id": m[1], "name": m[2], "cat": m[3], "sub": m[4],
                      "deps": deps, "cost": int(m[6])})
    return nodes


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("odmap")
    ap.add_argument("out")
    a = ap.parse_args()

    nodes = parse_research(RESEARCH_SRC.read_text(errors="replace"))
    if not nodes:
        print("no research nodes parsed -- has buildResearchNodes moved?",
              file=sys.stderr)
        return 1
    index = {n["id"]: i for i, n in enumerate(nodes)}

    z = zipfile.ZipFile(a.odmap)
    policies = json.loads(z.read("policies.json"))["policies"]
    claims = json.loads(z.read("claims.json"))
    countries = json.loads(z.read("countries.json"))
    iso_to_cid = {str(c.get("iso_a3", "")): int(k) for k, c in countries.items()}

    rows = []
    for iso, pids in claims.items():
        cid = iso_to_cid.get(iso)
        if cid is not None and pids:
            rows.append((cid, [int(p) for p in pids]))
    rows.sort()

    blob = bytearray()
    blob += MAGIC
    # ── MINORITIES ──
    #
    # Who lives where. The desktop game carries a full composition per
    # province; this keeps the four largest groups, which is enough to name
    # who is unhappy and to weight it, and turns 451 KB of JSON into 16.
    try:
        mins = json.loads(z.read("minorities.json"))
    except KeyError:
        mins = {}
    gname, gindex = [], {}
    mrows = []
    for pid, groups in sorted(mins.items(), key=lambda kv: int(kv[0])):
        if not isinstance(groups, list):
            continue
        top = sorted((g for g in groups if isinstance(g, dict)),
                     key=lambda g: -float(g.get("p", 0)))[:4]
        ent = []
        for g in top:
            nm = str(g.get("n", ""))[:19]
            if nm not in gindex:
                gindex[nm] = len(gname)
                gname.append(nm)
            ent.append((gindex[nm], min(int(float(g.get("p", 0))), 255)))
        while len(ent) < 4:
            ent.append((0xFF, 0))
        mrows.append((int(pid), ent))

    blob += struct.pack("<HHHHHH", VERSION, len(nodes), len(policies),
                        len(rows), len(gname), len(mrows))

    for n in nodes:
        cat = CATS.index(n["cat"]) if n["cat"] in CATS else 0xFF
        deps = [index[d] for d in n["deps"] if d in index][:MAX_DEPS]
        blob += fixed(n["id"], 12) + fixed(n["name"], 24) + fixed(n["sub"], 10)
        blob += struct.pack("<HBB", min(n["cost"], 0xFFFF), cat, len(deps))
        for i in range(MAX_DEPS):
            blob += struct.pack("<H", deps[i] if i < len(deps) else 0xFFFF)

    for p in policies:
        blob += fixed(str(p.get("id", "")), 16) + fixed(str(p.get("name", "")), 24)
        blob += fixed(str(p.get("category", "")), 10)
        blob += struct.pack("<HB B", min(int(p.get("cost_per_turn", 0) or 0), 0xFFFF),
                            min(int(p.get("implementation_turns", 0) or 0), 255), 0)
        lv = p.get("levers") or {}
        for name in LEVERS:
            v = int(round(float(lv.get(name, 0) or 0) * LEVER_SCALE))
            blob += struct.pack("<h", max(-32768, min(32767, v)))

    for cid, pids in rows:
        blob += struct.pack("<HH", cid, len(pids))
        for p in pids:
            blob += struct.pack("<I", p)

    for name, node, tk, pk, fd, idmg, fc, cost in ARTY:
        blob += fixed(name, 14) + fixed(node, 8)
        blob += struct.pack("<BBBBBBH", tk, pk, fd, idmg, fc, 0, cost)

    for nm in gname:
        blob += fixed(nm, 20)
    for pid, ent in mrows:
        blob += struct.pack("<I", pid)
        for gi, pct in ent:
            blob += struct.pack("<BB", gi & 0xFF, pct)

    pathlib.Path(a.out).write_bytes(blob)
    by_cat = {}
    for n in nodes:
        by_cat[n["cat"]] = by_cat.get(n["cat"], 0) + 1
    honoured = sum(1 for p in policies
                   if any(k in LEVERS for k in (p.get("levers") or {})))
    print(f"  {honoured} of {len(policies)} policies pull a lever this build "
          f"acts on; {len(LEVERS)} honoured, {len(LEVERS_SKIPPED)} recorded as "
          f"having nothing to act on")
    print(f"  {len(ARTY)} shell types, {len(gname)} ethnic groups across "
          f"{len(mrows)} provinces")
    print(f"{a.out}: {len(nodes)} research nodes, {len(policies)} policies, "
          f"{len(rows)} countries with claims "
          f"({sum(len(p) for _, p in rows)} provinces), {len(blob)/1024:.1f} KB")
    print("  research by category: " + ", ".join(f"{k} {v}" for k, v in sorted(by_cat.items())))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
