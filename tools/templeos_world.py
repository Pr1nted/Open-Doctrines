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

MAGIC, VERSION = b"ODTW", 8

# The record layouts, named once so the reader in templeos/World.HC can be
# checked against a single number instead of against a shape spread over three
# calls. PROV_REC and CTRY_REC are what OD_PREC and OD_CREC must equal there.
# army is 32-bit: a sixteen-bit field saturates at 65,535 and this game
# routinely garrisons millions, so India showed up in the claims panel with
# exactly 65535 men -- a number that looks like data and is a clamp.
PROV_FMT = "<IHHHIIHBBIIBBBB"
PROV_REC = 50   # the trailing pad became the port level          # PROV_FMT plus a 16-byte name
CTRY_FMT = "<HBB"
CTRY_REC = 34          # CTRY_FMT plus iso(4) treasury(2) rgb(4) name(20)


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
    try:
        ports = json.loads(z.read("ports.json"))
    except KeyError:
        ports = {}
    try:
        ships = json.loads(z.read("ships.json"))
    except KeyError:
        ships = []

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

    # ── AREA AT FULL RESOLUTION ──
    #
    # Not the downscaled pixel count. Open Doctrines' combat width is
    # max(20000, area x 25) in the units of the ORIGINAL province raster, so an
    # area measured on a 640-wide copy would put every province on the floor of
    # that formula and delete the mechanic. Counted on the source image, which
    # is the same number the real engine uses, so its constants transfer.
    full_area: dict[int, int] = {}
    for yy in range(sh):
        for xx in range(sw):
            r, g, b = px[xx, yy]
            pid = (r << 16) | (g << 8) | b
            if pid:
                full_area[pid] = full_area.get(pid, 0) + 1

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
    pop, army, income, forts, indlvl, resmask, portlvl = [], [], [], [], [], [], []
    for pid in order:
        v = population.get(str(pid), 0)
        pop.append(min(int(v) if isinstance(v, (int, float)) else 0, 0xFFFFFFFF))

        garrison = armies.get(str(pid), [])
        army.append(min(sum(int(g.get("count", 0)) for g in garrison
                            if isinstance(g, dict)), 0xFFFFFFFF))

        r = resources.get(str(pid), {})
        ind = r.get("industry", {}) if isinstance(r, dict) else {}
        total = sum(float(ind.get(k, 0) or 0)
                    for k in ("income", "resourceIncome", "popIncome"))
        income.append(min(int(total * 10), 0xFFFF))          # tenths, to stay integer
        forts.append(min(int(r.get("fortification", 0) or 0) if isinstance(r, dict) else 0, 255))
        indlvl.append(min(int(ind.get("level", 0) or 0), 255))

        # One bit per resource, in the order the desktop game lists them. A
        # deposit counts when either of its two figures is above zero, which
        # is what the engine's own "has any" test amounts to.
        mask = 0
        for bit, key in enumerate(("oil", "gold", "metal", "rubber", "gemstones")):
            d = r.get(key) if isinstance(r, dict) else None
            if isinstance(d, dict) and (float(d.get("a", 0) or 0) > 0
                                        or float(d.get("b", 0) or 0) > 0):
                mask |= 1 << bit
        resmask.append(mask)

        pt = ports.get(str(pid))
        portlvl.append(min(int(pt.get("level", 0)) if isinstance(pt, dict) else 0, 255))

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

    # Province names, for a panel that can say "Bavaria" instead of "#412".
    names = []
    for pid in order:
        nm = str(provinces.get(str(pid), {}).get("name", ""))[:15]
        names.append(nm.encode("ascii", "replace").ljust(16, b"\0"))

    cids = sorted({c for c in owner_cid if c in iso})

    blob = bytearray()
    blob += MAGIC
    # ── SHIPS, PUT WHERE THEY CAN BE FOUND ──
    #
    # ships.json places each hull at a latitude and longitude. Rather than
    # assume this raster's projection, every ship is assigned to the nearest
    # PORT its owner holds: the projection only has to be good enough to pick
    # between one country's harbours, and a fleet at its own dockyard is right
    # whichever way the map is drawn.
    port_of = {}
    for ix in range(n):
        if portlvl[ix] > 0:
            port_of.setdefault(owner_cid[ix], []).append(ix)

    fleet = []
    for sh in ships:
        if not isinstance(sh, dict):
            continue
        cid = int(sh.get("country_id", 0))
        cand = port_of.get(cid)
        if not cand:
            continue
        lon = float(sh.get("lon", 0.0))
        lat = float(sh.get("lat", 0.0))
        sx = int((lon + 180.0) / 360.0 * W)
        sy = int((90.0 - lat) / 180.0 * H)
        best = min(cand, key=lambda i: (cx[i] // max(cn[i], 1) - sx) ** 2
                   + (cy[i] // max(cn[i], 1) - sy) ** 2)
        kind = str(sh.get("type", ""))
        fleet.append((cid, kind, min(int(sh.get("health", 100)), 255), best))

    KINDS = ["boat", "destroyer", "cruiser", "submarine", "carrier",
             "battleship", "frigate"]

    # ── THE SEA, AS A COARSE GRID ──
    #
    # Ships need somewhere to go and a distance to cover getting there, and
    # the province graph cannot supply either: it is a land graph, and two
    # harbours on the same ocean are usually not neighbours in it at all.
    #
    # So the water is divided into 16-pixel cells, a cell counting as
    # navigable when it is at least half sea. That threshold does real work:
    # at this resolution the Panama isthmus is about two pixels wide, so its
    # cells are mostly land and the canal is correctly closed, while genuine
    # straits stay open. Measured on the shipped raster, the result is one
    # ocean -- 98% of navigable cells in a single connected body.
    #
    # Coarse on purpose. A per-pixel sea graph would be half a million nodes
    # for a machine that has to search it every time somebody moves a fleet.
    CELL = 16
    GW, GH = W // CELL, H // CELL
    nav = bytearray(GW * GH)
    for gy in range(GH):
        for gx in range(GW):
            sea = 0
            for yy in range(gy * CELL, gy * CELL + CELL):
                row = yy * W
                for xx in range(gx * CELL, gx * CELL + CELL):
                    if grid[row + xx] == SEA_IX:
                        sea += 1
            if sea * 2 >= CELL * CELL:
                nav[gy * GW + gx] = 1

    # Each harbour's cell: the nearest navigable one to its centre. A port
    # whose water is more than three cells away is not on this sea and is
    # left out rather than given a berth it cannot reach.
    port_cells = []
    for ix in range(n):
        if portlvl[ix] <= 0:
            continue
        px_ = cx[ix] // max(cn[ix], 1)
        py_ = cy[ix] // max(cn[ix], 1)
        gx0, gy0 = px_ // CELL, py_ // CELL
        best, bestd = -1, None
        for gy in range(max(0, gy0 - 3), min(GH, gy0 + 4)):
            for gx in range(max(0, gx0 - 3), min(GW, gx0 + 4)):
                if nav[gy * GW + gx]:
                    d = (gx - gx0) ** 2 + (gy - gy0) ** 2
                    if bestd is None or d < bestd:
                        best, bestd = gy * GW + gx, d
        if best >= 0:
            port_cells.append((ix, best))

    blob += struct.pack("<HHHHHIIHHHH", VERSION, W, H, n, len(cids),
                        len(runs), len(adj_flat) // 2, len(fleet),
                        GW, GH, len(port_cells))
    blob += runs
    # ── ONE PROVINCE RECORD, 44 BYTES, WRITTEN IN ONE PLACE ──
    #
    # Assembled as a single pack rather than three appended ones. Two earlier
    # versions of this file were edited by pattern-matching the middle of a
    # multi-line pack, the pattern stopped matching, and the fields were
    # silently dropped: forts were written as a constant zero and the area and
    # the names never reached the file at all. It still loaded, still reported
    # 1,615 provinces, and every province was unfortified. One call, one
    # format string, one length the reader can be checked against.
    assert struct.calcsize(PROV_FMT) + 16 == PROV_REC
    for ix in range(n):
        blob += struct.pack(PROV_FMT, order[ix],
                            cx[ix] // max(cn[ix], 1), cy[ix] // max(cn[ix], 1),
                            owner_cid[ix], pop[ix], army[ix], income[ix],
                            min(adj_n[ix], 255), forts[ix],
                            adj_off[ix],
                            min(full_area.get(order[ix], 1), 0xFFFFFFFF),
                            indlvl[ix], resmask[ix], portlvl[ix], 0)
        blob += names[ix]
    blob += adj_flat
    for cid in cids:
        treasury = 0
        c = countries.get(str(cid), {})
        try:
            treasury = min(int(float(c.get("treasury", 0)) * 10), 0xFFFF)
        except (TypeError, ValueError):
            pass
        # The palette index AND the true colour. The index is what a sixteen
        # colour screen needs; the rgb is what the real game looks like, and
        # the machine can be talked into showing it.
        rgb = (128, 128, 128)
        col = c.get("color", "")
        if isinstance(col, str) and col.startswith("#") and len(col) >= 7:
            rgb = tuple(int(col[i:i + 2], 16) for i in (1, 3, 5))
        rec = struct.pack(CTRY_FMT, cid, colour.get(cid, 8), 0)
        rec += iso[cid].encode("ascii").ljust(4, b"\0")
        rec += struct.pack("<H", treasury)
        rec += struct.pack("<BBBB", rgb[0], rgb[1], rgb[2], 0)
        rec += str(c.get("name", iso[cid]))[:19].encode("ascii", "replace").ljust(20, b"\0")
        assert len(rec) == CTRY_REC
        blob += rec

    for cid, kind, health, ix in fleet:
        k = KINDS.index(kind) if kind in KINDS else 0
        blob += struct.pack("<HBBH", cid & 0xFFFF, k, health, ix)

    blob += bytes(nav)
    for ix, cell in port_cells:
        blob += struct.pack("<HH", ix, cell)

    pathlib.Path(a.out).write_bytes(blob)
    # The reader's arithmetic, done here: if this does not land exactly on the
    # end of the file, the two sides disagree about a record size and every
    # country will come out as garbage.
    expect = (30 + len(runs) + n * PROV_REC + len(adj_flat)
              + len(cids) * CTRY_REC + len(fleet) * 6
              + GW * GH + len(port_cells) * 4)
    assert expect == len(blob), f"layout mismatch: {expect} computed, {len(blob)} written"

    print(f"{a.out}: {W}x{H}, {n} provinces, {len(cids)} countries, "
          f"{len(runs) // 3} runs, {len(adj_flat) // 2} adjacencies, "
          f"{len(blob) / 1024:.0f} KB")
    print(f"  population {sum(pop) / 1e6:.0f}M, armies {sum(army):,}, "
          f"income {sum(income) / 10:.0f}/turn, "
          f"{sum(1 for f in forts if f)} fortified, "
          f"{sum(1 for m in resmask if m)} with deposits, "
          f"{sum(1 for v in portlvl if v)} ports, {len(fleet)} ships")
    print(f"  sea grid {GW}x{GH}, {sum(nav)} navigable cells, "
          f"{len(port_cells)} harbours on the water")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
