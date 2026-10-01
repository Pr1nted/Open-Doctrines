#!/usr/bin/env python3
"""No two research nodes may overlap in the tree that draws them.

    python3 tools/check_research_layout.py

WHY THIS EXISTS

The research screen draws each node as a box of baseNodeW x baseNodeH at the
(x, y) written in buildResearchNodes(). Nothing checked that two boxes in the
same tree were far enough apart, and the monuments tree had three nodes on one
row at x = 380, 480 and 640 -- a 100-point gap between 160-point boxes. Two of
them overlapped outright and the third sat flush against its neighbour. It
looked, in the player's words, "very funky", and no test could see it because
the coordinates are literals in a function that needs the whole game to link.

So this reads the literals. It is a lint over source text, which is a weaker
instrument than linking the real function -- but it is the one available
without pulling Game.h into a test binary, and it catches exactly the mistake
that was made.

GUARDED AGAINST ITS OWN REGEX ROTTING. A parser that silently stops matching
reports a clean tree forever, which is worse than no check. If the node count
falls below what the file plainly contains, this fails and says so rather than
passing on nothing.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "Game_Research.cpp")
RENDER = os.path.join(ROOT, "src", "Game_Research.cpp")

# If the file ever holds fewer than this, the parse has broken rather than the
# trees having shrunk by two thirds.
MIN_NODES = 70


def node_box():
    """baseNodeW / baseNodeH, read from the drawing rather than assumed."""
    txt = open(RENDER, encoding="utf-8").read()
    w = re.search(r"const int baseNodeW\s*=\s*(\d+)", txt)
    h = re.search(r"const int baseNodeH\s*=\s*(\d+)", txt)
    if not w or not h:
        print("FAIL  could not read baseNodeW/baseNodeH from Game_Research.cpp")
        sys.exit(1)
    return int(w.group(1)), int(h.group(1))


def nodes():
    """(id, category, subcategory, x, y) for every add() in the file."""
    txt = open(SRC, encoding="utf-8").read()
    out = []
    # add("id", "name", "desc", "cat", "subcat", {deps}, cost, x, y)
    pat = re.compile(
        r'add\(\s*"([^"]+)"\s*,\s*"(?:[^"\\]|\\.)*"\s*,\s*(?:"(?:[^"\\]|\\.)*"\s*)+,\s*'
        r'"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*\{[^}]*\}\s*,\s*-?\d+\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\)',
        re.S)
    for m in pat.finditer(txt):
        out.append((m.group(1), m.group(2), m.group(3), int(m.group(4)), int(m.group(5))))
    # The monuments helper names its node from the catalogue, so its id is not
    # a literal -- the Kind is, and it is unique, which is all this needs.
    monpat = re.compile(
        r'mon\(\s*odmon::Kind::(\w+)\s*,\s*"([^"]+)"\s*,\s*\{[^}]*\}\s*,\s*-?\d+\s*,'
        r'\s*(-?\d+)\s*,\s*(-?\d+)\s*\)', re.S)
    for m in monpat.finditer(txt):
        out.append((m.group(1), "monuments", m.group(2), int(m.group(3)), int(m.group(4))))
    return out


def main():
    w, h = node_box()
    ns = nodes()
    print("node box %dx%d, %d nodes parsed" % (w, h, len(ns)))
    if len(ns) < MIN_NODES:
        print("FAIL  only %d nodes parsed; expected at least %d -- the parser has "
              "stopped matching, which would report a clean tree for ever"
              % (len(ns), MIN_NODES))
        return 1

    # Trees are drawn one category at a time, so only nodes sharing a category
    # can collide on screen.
    bad = 0
    by_cat = {}
    for n in ns:
        by_cat.setdefault(n[1], []).append(n)
    for cat, group in sorted(by_cat.items()):
        hits = 0
        for i in range(len(group)):
            for j in range(i + 1, len(group)):
                a, b = group[i], group[j]
                if abs(a[3] - b[3]) < w and abs(a[4] - b[4]) < h:
                    print("  FAIL  %s: %s (%d,%d) overlaps %s (%d,%d)"
                          % (cat, a[0], a[3], a[4], b[0], b[3], b[4]))
                    hits += 1
        bad += hits
        # Only when there were none. Printing "ok, no overlaps" straight after
        # listing one is the kind of report that gets believed over its own
        # evidence.
        if not hits:
            print("  ok    %-12s %2d nodes, no overlaps" % (cat, len(group)))

    if bad:
        print("\n%d overlapping pair(s). Boxes are %dx%d, so nodes need that much "
              "clear between them." % (bad, w, h))
        return 1
    print("\nok  no research node overlaps another in its tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
