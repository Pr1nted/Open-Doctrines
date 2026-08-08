#!/usr/bin/env python3
"""
Re-derive the lon/lat anchors in fix_map_history.py from the maps as they are.

    python3 tools/reanchor_map_history.py --check   # report drift, change nothing
    python3 tools/reanchor_map_history.py           # rewrite the tables in place

WHEN YOU NEED THIS

Almost never, and that is the point. tools/fix_map_history.py names every
province it touches by a point on the Earth rather than by a province id,
precisely so that regenerating the world does not invalidate the table. An
anchor keeps pointing at the same ground however the province layer is re-cut
around it.

The exception is a re-cut big enough to move an anchor into a DIFFERENT
country: merge two provinces across a border, or redraw a coastline by more
than the anchor's clearance, and a point that used to sit deep inside Bhutan
can end up just inside British India. fix_map_history reports that as a
province it did not expect, and this is what fixes it.

Run it against maps you have checked by eye and believe are right. It reads the
current .odmap files and rewrites each anchor to the DEEPEST INTERIOR POINT of
the province that entry currently affects -- the point furthest from any of
that province's boundaries, which is the position most likely to survive the
next re-cut.

WHAT IT WILL NOT DO

Decide which province an entry SHOULD name. If an anchor has drifted into the
wrong country, re-anchoring it there makes the mistake permanent and tidy.
Read the report first; that is why --check exists and why it prints the country
each anchor currently lands in.
"""

import argparse
import ast
import io
import os
import sys
import zipfile

import numpy as np
from PIL import Image

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS_DIR)
SRC = os.path.join(TOOLS_DIR, "fix_map_history.py")
MAPS_DIR = os.path.join(ROOT, "data", "STDmaps")


def load_map(name):
    z = zipfile.ZipFile(os.path.join(MAPS_DIR, name))
    import json
    pv = np.array(Image.open(io.BytesIO(z.read("provinces.png"))).convert("RGB"),
                  dtype=np.uint32)
    pid = pv[:, :, 0] << 16 | pv[:, :, 1] << 8 | pv[:, :, 2]
    return pid, json.loads(z.read("provinces.json"))


def deepest_point(pid_arr, pid):
    """The pixel of `pid` furthest from any of its boundaries, as lon/lat.

    Repeated erosion of the province mask; the last non-empty result is its
    core. A centroid is not usable here -- for a crescent-shaped province it
    lands outside the province altogether -- and a boundary pixel is exactly
    the thing that ends up on the wrong side of the next re-cut.
    """
    h, w = pid_arr.shape
    ys, xs = np.nonzero(pid_arr == pid)
    if not len(ys):
        return None
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    cur = pid_arr[y0:y1, x0:x1] == pid
    best = cur
    while True:
        e = cur.copy()
        e[1:, :] &= cur[:-1, :]
        e[:-1, :] &= cur[1:, :]
        e[:, 1:] &= cur[:, :-1]
        e[:, :-1] &= cur[:, 1:]
        e[0, :] = False
        e[-1, :] = False
        e[:, 0] = False
        e[:, -1] = False
        if not e.any():
            break
        best, cur = e, e
    by, bx = np.nonzero(best)
    i = len(by) // 2
    y, x = int(by[i]) + y0, int(bx[i]) + x0
    return (round((x + 0.5) / w * 360.0 - 180.0, 4),
            round(90.0 - (y + 0.5) / h * 180.0, 4))


def spans(src):
    """Every anchor list in PLAN and REASSIGN, with its source span."""
    lines = src.splitlines(keepends=True)
    off = [0]
    for L in lines:
        off.append(off[-1] + len(L))

    def span(n):
        return (off[n.lineno - 1] + n.col_offset,
                off[n.end_lineno - 1] + n.end_col_offset)

    tree = ast.parse(src)
    plan = reassign = None
    for n in tree.body:
        if isinstance(n, ast.Assign) and isinstance(n.targets[0], ast.Name):
            if n.targets[0].id == "PLAN":
                plan = n.value
            elif n.targets[0].id == "REASSIGN":
                reassign = n.value

    jobs = []
    for k, v in zip(plan.keys, plan.values):
        for iso, spec in zip(v.keys, v.values):
            for kw in spec.keywords:
                if kw.arg == "at":
                    jobs.append((k.value, iso.value, kw.value, span(kw.value)))
    for k, v in zip(reassign.keys, reassign.values):
        for tup in v.elts:
            label = f"{tup.elts[1].value}->{tup.elts[2].value}"
            jobs.append((k.value, label, tup.elts[0], span(tup.elts[0])))
    return jobs


def fmt(points, indent):
    pieces = [f"({lon}, {lat})" for lon, lat in points]
    body = ", ".join(pieces)
    if len(body) + indent <= 74:
        return "[" + body + "]"
    chunks, cur = [], ""
    for p in pieces:
        if cur and len(cur) + len(p) + 2 > 66:
            chunks.append(cur)
            cur = p
        else:
            cur = f"{cur}, {p}" if cur else p
    chunks.append(cur)
    return "[" + (",\n" + " " * (indent + 1)).join(chunks) + "]"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="report where each anchor lands; exit non-zero if any moved")
    args = ap.parse_args()

    src = open(SRC, encoding="utf-8").read()
    jobs = spans(src)
    cache = {}
    edits, drifted = [], 0

    for mapname, label, node, (s, e) in jobs:
        if mapname not in cache:
            if not os.path.exists(os.path.join(MAPS_DIR, mapname)):
                print(f"  {mapname}: not built, skipped")
                cache[mapname] = None
            else:
                cache[mapname] = load_map(mapname)
        if cache[mapname] is None:
            continue
        pid_arr, provinces = cache[mapname]
        h, w = pid_arr.shape

        # literal_eval, not `.value`: a negative longitude parses as a UnaryOp
        # wrapping a Constant, so reading .value off the element itself works
        # everywhere east of Greenwich and raises everywhere west of it.
        new, seen = [], set()
        for el in node.elts:
            lon, lat = ast.literal_eval(el)
            x = int((lon + 180.0) / 360.0 * w) % w
            y = min(h - 1, max(0, int((90.0 - lat) / 180.0 * h)))
            pid = int(pid_arr[y, x])
            if pid == 0:
                print(f"  {mapname} {label}: anchor {lon},{lat} is open sea "
                      f"-- fix this by hand, not here")
                new.append((lon, lat))
                drifted += 1
                continue
            deep = deepest_point(pid_arr, pid)
            owner = provinces.get(str(pid), {}).get("iso_a3", "?")
            if pid in seen:
                # Two anchors, one province: a re-cut merged what used to be
                # two. Collapse them, or every future run warns about a
                # duplicate that this tool put there.
                drifted += 1
                print(f"  {mapname} {label}: {lon},{lat} dropped -- province "
                      f"{pid} is already named by an earlier anchor")
                continue
            seen.add(pid)
            if deep != (lon, lat):
                drifted += 1
                print(f"  {mapname} {label}: {lon},{lat} -> {deep[0]},{deep[1]} "
                      f"(province {pid}, now {owner})")
            new.append(deep or (lon, lat))
        if new != [tuple(ast.literal_eval(el)) for el in node.elts]:
            edits.append((s, e, fmt(new, node.col_offset)))

    if args.check:
        if drifted:
            print(f"\n{drifted} anchor(s) are no longer at their province's core. "
                  f"Read the list above -- if every one still names the country "
                  f"you meant, run without --check.")
            return 1
        print("Every anchor is still at the deepest interior point of the "
              "province it names.")
        return 0

    for s, e, rep in sorted(edits, key=lambda t: -t[0]):
        src = src[:s] + rep + src[e:]
    open(SRC, "w", encoding="utf-8").write(src)
    print(f"\nrewrote {len(edits)} anchor list(s) in {SRC}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
