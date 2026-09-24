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

MAGIC, VERSION = b"ODTD", 1
MAX_DEPS = 4

# The five categories the research tree uses, as fixed slots so the guest can
# group without carrying strings it would only compare.
CATS = ["buildings", "army", "formations", "population", "efficiency", "misc"]


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
        r'"([^"]*)"\s*,\s*"[^"]*"\s*,\s*\{([^}]*)\}\s*,\s*(\d+)', re.S)
    for m in pat.finditer(body):
        deps = re.findall(r'"([^"]+)"', m[4])
        nodes.append({"id": m[1], "name": m[2], "cat": m[3],
                      "deps": deps, "cost": int(m[5])})
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
    blob += struct.pack("<HHHH", VERSION, len(nodes), len(policies), len(rows))

    for n in nodes:
        cat = CATS.index(n["cat"]) if n["cat"] in CATS else 0xFF
        deps = [index[d] for d in n["deps"] if d in index][:MAX_DEPS]
        blob += fixed(n["id"], 12) + fixed(n["name"], 24)
        blob += struct.pack("<HBB", min(n["cost"], 0xFFFF), cat, len(deps))
        for i in range(MAX_DEPS):
            blob += struct.pack("<H", deps[i] if i < len(deps) else 0xFFFF)

    for p in policies:
        blob += fixed(str(p.get("id", "")), 16) + fixed(str(p.get("name", "")), 24)
        blob += fixed(str(p.get("category", "")), 10)
        blob += struct.pack("<HB B", min(int(p.get("cost_per_turn", 0) or 0), 0xFFFF),
                            min(int(p.get("implementation_turns", 0) or 0), 255), 0)

    for cid, pids in rows:
        blob += struct.pack("<HH", cid, len(pids))
        for p in pids:
            blob += struct.pack("<I", p)

    pathlib.Path(a.out).write_bytes(blob)
    by_cat = {}
    for n in nodes:
        by_cat[n["cat"]] = by_cat.get(n["cat"], 0) + 1
    print(f"{a.out}: {len(nodes)} research nodes, {len(policies)} policies, "
          f"{len(rows)} countries with claims "
          f"({sum(len(p) for _, p in rows)} provinces), {len(blob)/1024:.1f} KB")
    print("  research by category: " + ", ".join(f"{k} {v}" for k, v in sorted(by_cat.items())))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
