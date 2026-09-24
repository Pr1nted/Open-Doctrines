#!/usr/bin/env python3
"""Bake an .odmap into a world TempleOS can play on its own.

The map exporter that came before this one produced a picture: pixels already
resolved to colours. That is enough to look at and useless to play on, because
a province that changes hands cannot be repainted -- the colour has been baked
in and the province is gone.

So this ships the WORLD, not a view of it:

  * a raster of province indices, run-length encoded, from which the guest
    paints its own map and can repaint it whenever an owner changes,
  * every province's centre, population, army, income and NEIGHBOURS, which is
    what makes it a board rather than an image,
  * every country's code, colour and treasury.

Nothing here is a rendering decision; those all move to the guest, which is the
point. About 150 KB for a 1,480-province world.

    python3 tools/templeos_world.py data/STDmaps/map.odmap templeos/world.odw
"""
from __future__ import annotations

import argparse
import json
import pathlib
import struct
import sys
import zipfile

# Adam/Gr/GrPalette.HC, gr_palette_std -- red in the top 16 bits of each entry.
PALETTE = [
    (0x00, 0x00, 0x00), (0x00, 0x00, 0xAA), (0x00, 0xAA, 0x00), (0x00, 0xAA, 0xAA),
    (0xAA, 0x00, 0x00), (0xAA, 0x00, 0xAA), (0xAA, 0x55, 0x00), (0xAA, 0xAA, 0xAA),
    (0x55, 0x55, 0x55), (0x55, 0x55, 0xFF), (0x55, 0xFF, 0x55), (0x55, 0xFF, 0xFF),
    (0xFF, 0x55, 0x55), (0xFF, 0x55, 0xFF), (0xFF, 0xFF, 0x55), (0xFF, 0xFF, 0xFF),
]
SEA_IX = 0xFFFF                  # the province index that means "no province"
BORDER, SEA_COLOUR = 0, 1        # BLACK, BLUE
LAND = [i for i in range(16) if i not in (SEA_COLOUR, BORDER)]

MAGIC, VERSION = b"ODTW", 3


def nearest(rgb):
    r, g, b = rgb
    return min(LAND, key=lambda i: (r - PALETTE[i][0]) ** 2
               + (g - PALETTE[i][1]) ** 2 + (b - PALETTE[i][2]) ** 2)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("odmap")
    ap.add_argument("out")
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=360)
    a = ap.parse_args()

    try:
        from PIL import Image
    except ImportError:
        print("this needs Pillow", file=sys.stderr)
        return 1

    z = zipfile.ZipFile(a.odmap)
    with z.open("provinces.png") as f:
        src = Image.open(f).convert("RGB")
        src.load()
    provinces = json.loads(z.read("provinces.json"))
    countries = json.loads(z.read("countries.json"))
    try:
        population = json.loads(z.read("population.json"))
    except KeyError:
        population = {}
    try:
        armies = json.loads(z.read("armies.json"))
    except KeyError:
        armies = {}
    try:
        resources = json.loads(z.read("resources.json"))
    except KeyError:
        resources = {}

    W, H = a.width, a.height
    sw, sh = src.size
    px = src.load()

    # ── the raster, as province indices ──
    #
    # Sampled on ids, never on colours: averaging a province raster invents ids
    # that do not exist. Every province that survives the downscale gets an
    # index; the guest never sees the real ids except to display them.
    seen: dict[int, int] = {}
    order: list[int] = []
    grid = [SEA_IX] * (W * H)
    for y in range(H):
        syy = y * sh // H
        for x in range(W):
            r, g, b = px[x * sw // W, syy]
            pid = (r << 16) | (g << 8) | b
            if pid == 0:
                continue
            ix = seen.get(pid)
            if ix is None:
                ix = seen[pid] = len(order)
                order.append(pid)
            grid[y * W + x] = ix

    n = len(order)
    cx = [0] * n; cy = [0] * n; cn = [0] * n
    adj: list[set] = [set() for _ in range(n)]
    for y in range(H):
        row = y * W
        for x in range(W):
            ix = grid[row + x]
            if ix == SEA_IX:
                continue
            cx[ix] += x; cy[ix] += y; cn[ix] += 1
            for nx, ny in ((x + 1, y), (x, y + 1)):
                if nx < W and ny < H:
                    j = grid[ny * W + nx]
                    if j != SEA_IX and j != ix:
                        adj[ix].add(j); adj[j].add(ix)

    # ── owners, and a colour per country that differs from its neighbours ──
    owner_cid = []
    for pid in order:
        p = provinces.get(str(pid), {})
        owner_cid.append(int(p.get("country_id", 0)) & 0xFFFF)

    cadj: dict[int, set] = {}
    for ix in range(n):
        for j in adj[ix]:
            a_, b_ = owner_cid[ix], owner_cid[j]
            if a_ != b_:
                cadj.setdefault(a_, set()).add(b_)
                cadj.setdefault(b_, set()).add(a_)

    want, iso = {}, {}
    for k, c in countries.items():
        col = c.get("color", "")
        if isinstance(col, str) and col.startswith("#") and len(col) >= 7:
            want[int(k)] = nearest(tuple(int(col[i:i + 2], 16) for i in (1, 3, 5)))
        code = str(c.get("iso_a3", ""))[:3]
        if code:
            iso[int(k)] = code

    # Four colours suffice for a planar map and there are fourteen; greedy in
    # descending degree, preferring the country's own hue where it is free.
    colour: dict[int, int] = {}
    for cid in sorted(cadj, key=lambda c: -len(cadj[c])):
        taken = {colour[x] for x in cadj[cid] if x in colour}
        free = [c for c in LAND if c not in taken] or LAND
        colour[cid] = min(free, key=lambda c: abs(c - want.get(cid, 8)))
    for cid in want:
        colour.setdefault(cid, want[cid])

    # ── per-province numbers ──
    #
    # Each of these is shaped differently and none of them is a plain number,
    # which is worth saying because the first version of this reached for
    # `["army"]` and `["income"]`, found neither, and produced a world with no
    # soldiers and no economy that otherwise looked completely fine.
    #
    #   population.json  pid -> PEOPLE, as a bare number (the world sums to
    #                    6.1e9, which is how you can tell it is not thousands)
    #   armies.json      pid -> [{country_id, count}, ...]
    #   resources.json   pid -> {"industry": {income, resourceIncome, popIncome}}
    pop, army, income, forts = [], [], [], []
    for pid in order:
        v = population.get(str(pid), 0)
        pop.append(min(int(v) if isinstance(v, (int, float)) else 0, 0xFFFFFFFF))

        garrison = armies.get(str(pid), [])
        army.append(min(sum(int(g.get("count", 0)) for g in garrison
                            if isinstance(g, dict)), 0xFFFF))

        r = resources.get(str(pid), {})
        ind = r.get("industry", {}) if isinstance(r, dict) else {}
        total = sum(float(ind.get(k, 0) or 0)
                    for k in ("income", "resourceIncome", "popIncome"))
        income.append(min(int(total * 10), 0xFFFF))          # tenths, to stay integer
        forts.append(min(int(r.get("fortification", 0) or 0) if isinstance(r, dict) else 0, 255))

    # ── run-length encode the index raster ──
    runs = bytearray()
    cur, run = grid[0], 1
    for v in grid[1:]:
        if v == cur and run < 255:
            run += 1
        else:
            runs += struct.pack("<BH", run, cur)
            cur, run = v, 1
    runs += struct.pack("<BH", run, cur)

    # ── adjacency, flattened ──
    adj_flat = bytearray()
    adj_off, adj_n = [], []
    for ix in range(n):
        lst = sorted(adj[ix])[:255]
        adj_off.append(len(adj_flat) // 2)
        adj_n.append(len(lst))
        for j in lst:
            adj_flat += struct.pack("<H", j)

    cids = sorted({c for c in owner_cid if c in iso})

    blob = bytearray()
    blob += MAGIC
    blob += struct.pack("<HHHHHII", VERSION, W, H, n, len(cids),
                        len(runs), len(adj_flat) // 2)
    blob += runs
    for ix in range(n):
        blob += struct.pack("<IHHHIHHBB", order[ix],
                            cx[ix] // max(cn[ix], 1), cy[ix] // max(cn[ix], 1),
                            owner_cid[ix], pop[ix], army[ix], income[ix],
                            min(adj_n[ix], 255), 0)
        blob += struct.pack("<I", adj_off[ix])
    blob += adj_flat
    for cid in cids:
        treasury = 0
        c = countries.get(str(cid), {})
        try:
            treasury = min(int(float(c.get("treasury", 0)) * 10), 0xFFFF)
        except (TypeError, ValueError):
            pass
        blob += struct.pack("<HBB", cid, colour.get(cid, 8), 0)
        blob += iso[cid].encode("ascii").ljust(4, b"\0")
        blob += struct.pack("<H", treasury)

    pathlib.Path(a.out).write_bytes(blob)
    print(f"{a.out}: {W}x{H}, {n} provinces, {len(cids)} countries, "
          f"{len(runs) // 3} runs, {len(adj_flat) // 2} adjacencies, "
          f"{len(blob) / 1024:.0f} KB")
    print(f"  population {sum(pop) / 1e6:.0f}M, armies {sum(army):,}, "
          f"income {sum(income) / 10:.0f}/turn, "
          f"{sum(1 for f in forts if f)} fortified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
