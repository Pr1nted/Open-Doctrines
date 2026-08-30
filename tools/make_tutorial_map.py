#!/usr/bin/env python3
"""Build the tutorial world: two islands, authored rather than generated.

    python3 tools/make_tutorial_map.py

WHY THIS IS NOT PROCEDURAL. Every other map is generated from a land/sea
drawing and then edited by hand. The tutorial cannot be, because the tutorial
is a script and the script names things. Mia says "land your army on the far
island", and something has to guarantee there IS a far island, that Ashford
owns a port on this side of the water, and that the country over there is
small enough to lose. A seed that produces a good world today produces a
different one the moment the generator changes.

So the geography is written down here. Province ids are stable, which is what
lets the tutorial point at a province and mean it.

THE SHAPE OF IT

  Ashford (the player) and Kestrel share the western island. Kestrel is the
  land lesson: reachable on foot, and weak.
  Verrick has the eastern island to itself. It is the naval lesson -- there is
  no way to reach it at all without transports.
  Hallow Rock sits in the strait between them: one province, Verrick's, and
  the obvious first hop. A crossing taught in two short moves is a crossing a
  player can see the shape of; taught in one long one it is a leap of faith.

The water between the islands is a STRAIT, not an ocean. Wide enough that no
army walks it, narrow enough to read as a place with two sides -- which is
what makes "the far shore" mean anything on screen.

The rebellion is NOT in this file. The tutorial stages it at the turn it wants
one: a rebellion that exists at load is one the player can lose to before
being told what unrest is.
"""
import json
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from odmap_pack import layer_png, write_odmap

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "data", "STDmaps", "tutorial.odmap")
DONOR = os.path.join(ROOT, "data", "STDmaps", "1939.odmap")

# The archive's layers are full size; the geography is worked out on a coarse
# grid and scaled up, exactly as the procedural path does. Nothing here wants
# eight thousand pixels of precision, and a 33-megapixel Voronoi is a lot of
# memory for a coastline nobody will measure.
FULL_W, FULL_H = 8192, 4096
W, H = 1024, 512
K = FULL_W // W

# A BOAT CAN ONLY REACH 200 MAP PIXELS, and it is measured to the target
# province's CENTRE, not to its beach -- see shipMaxRangePx and the disembark
# resolver in Game_TurnLogic.cpp.
#
# The first version of this island pair ignored that completely. The strait
# was 960 px of open water and Verrick's nearest province centre sat 637 px
# from where a boat could sit, so the crossing the whole naval lesson builds
# to was not merely hard, it was arithmetically impossible: every province on
# the far island reported "out of range" and the order was refused.
#
# Everything below is therefore sized in units of that range. The islands are
# small, and they are close.
BOAT_RANGE_PX = 200.0
BOAT_RANGE = BOAT_RANGE_PX / K          # 25 coarse cells

ASHFORD, KESTREL, VERRICK = 1, 2, 3
WEST, EAST, ROCK = "west", "east", "rock"
COUNTRIES = {
    ASHFORD: dict(iso="ASH", name="Ashford", color="#4a7fb5",
                  flag=["#1d3557", "#f1faee", "#4a7fb5"], banner="bands",
                  treasury=140.0, compass=dict(left=10, auth=-5)),
    KESTREL: dict(iso="KES", name="Kestrel", color="#b5563f",
                  flag=["#6a1b1b", "#e8c07d", "#b5563f"], banner="chevron",
                  treasury=60.0, compass=dict(left=-30, auth=70)),
    VERRICK: dict(iso="VER", name="Verrick", color="#5f9e5a",
                  flag=["#24421f", "#f0efe2", "#5f9e5a"], banner="canton",
                  treasury=75.0, compass=dict(left=40, auth=25)),
}

# HOW THE PROVINCES ARE PLACED.
#
# By hand, once -- and every time the islands were resized the coordinates
# stopped being on them. Seeds fell in the sea, provinces came out empty, and
# each fix moved the geometry again. So they are SAMPLED from the land
# instead: spread evenly inside whatever shape the islands currently are.
#
# Two of them are pinned, because the naval lesson depends on exactly those:
# the tip of each island facing the strait. A landing is refused unless the
# target province's CENTRE is within a boat's range, so the far side needs a
# province whose centre is near the water rather than in the middle of the
# island.
COUNTS = {  # landmass -> how many provinces to sample
    "west": 11,     # split between Ashford and Kestrel by longitude
    "east": 8,
    "rock": 1,
}
KESTREL_SHARE = 4   # the easternmost N of the western island are Kestrel's


def _inland(mask, cells=3):
    """`mask` minus a rim `cells` deep, or the whole thing if that empties it.

    Farthest-point sampling always reaches for an extremity, and on an island
    every extremity is a headland -- so seeding from the raw mask put province
    centres on the coastline and produced slivers a few hundred pixels across.
    Seeds come from the interior; the coast still gets divided up, it simply
    is not where the centres are.
    """
    m = mask.copy()
    for _ in range(cells):
        e = m.copy()
        e[1:, :] &= m[:-1, :]
        e[:-1, :] &= m[1:, :]
        e[:, 1:] &= m[:, :-1]
        e[:, :-1] &= m[:, 1:]
        if not e.any():
            return m
        m = e
    return m


def sample_seeds(mask, n, pinned=()):
    """N well-spread points inside `mask`, farthest-point sampled.

    Deterministic: it starts from the pinned points (or the westernmost land)
    and each new seed is the land cell furthest from every seed so far, so the
    same island always yields the same provinces.
    """
    ys, xs = np.nonzero(_inland(mask))
    pts = [(int(px), int(py)) for px, py in pinned if mask[int(py), int(px)]]
    if not pts:
        k = int(np.argmin(xs))
        pts = [(int(xs[k]), int(ys[k]))]
    d2 = np.full(xs.shape, np.inf)
    for (px, py) in pts:
        d2 = np.minimum(d2, (xs - px) ** 2 + (ys - py) ** 2)
    while len(pts) < n:
        k = int(np.argmax(d2))
        pts.append((int(xs[k]), int(ys[k])))
        d2 = np.minimum(d2, (xs - xs[k]) ** 2 + (ys - ys[k]) ** 2)
    return pts


def build_seeds(masks):
    """(x, y, country, landmass) for every province, in id order."""
    out = []
    west = masks[WEST]
    wys, wxs = np.nonzero(west)
    # Pin the eastern tip: Kestrel's landing shore, and the ground the player
    # sails FROM once they have taken it.
    tip = int(np.argmax(wxs))
    # The pinned tip is pulled a little inland for the same reason: a centre
    # sitting exactly on the shore makes a province that is nearly all coast.
    wpts = sample_seeds(west, COUNTS["west"],
                        pinned=[(wxs[tip] - 6, wys[tip])])
    wpts.sort(key=lambda p: p[0])                 # west to east
    for i, (x, y) in enumerate(wpts):
        cid = KESTREL if i >= COUNTS["west"] - KESTREL_SHARE else ASHFORD
        out.append((x, y, cid, WEST))
    rys, rxs = np.nonzero(masks[ROCK])
    out.append((int(rxs.mean()), int(rys.mean()), VERRICK, ROCK))
    east = masks[EAST]
    eys, exs = np.nonzero(east)
    wtip = int(np.argmin(exs))                    # the shore facing the strait
    # Pinned only just inland. A landing is refused on distance to the
    # province CENTRE, so the province facing the strait has to have its
    # centre near the water -- three cells, not a comfortable dozen.
    for (x, y) in sample_seeds(east, COUNTS["east"],
                               pinned=[(exs[wtip] + 3, eys[wtip])]):
        out.append((x, y, VERRICK, EAST))
    return out


SEEDS = []          # filled by build_seeds once the islands are known

def flag_svg(kind, colors, w=240, h=144):
    """A flag for an invented country, drawn from primitives.

    SVG, NOT PNG. A map archive only ever unpacks flags/*.svg -- see the entry
    filter in Game_Loading.cpp -- so a PNG embedded next to it is carried
    around and never looked at, and the country renders as a grey rectangle.
    (The .png paths in the shipped maps resolve against data/flags/ on disk,
    which is a different mechanism and not one a self-contained map can use.)

    DELIBERATELY ABSTRACT. Bands, a canton, a chevron -- shapes with no
    referent. Three countries that exist only to be practised on do not need
    heraldry, and anything drawn from real vexillary vocabulary risks landing
    on somebody's actual symbol by accident; a tutorial is the last place to
    be explaining that. No runes, no crosses, no stars, no animals.
    """
    a, b, c = colors
    parts = [f'<rect width="{w}" height="{h}" fill="{a}"/>']
    if kind == "bands":                       # three horizontals, wide centre
        parts.append(f'<rect y="{h//4}" width="{w}" height="{h//2}" fill="{b}"/>')
        parts.append(f'<rect y="{h//2 - h//16}" width="{w}" height="{h//8}" fill="{c}"/>')
    elif kind == "canton":                    # a block in the hoist, one bar
        parts.append(f'<rect width="{w//2}" height="{h//2}" fill="{b}"/>')
        parts.append(f'<rect y="{h - h//5}" width="{w}" height="{h//5}" fill="{c}"/>')
    elif kind == "chevron":                   # a wedge from the hoist
        parts.append(f'<polygon points="0,0 {w//2},{h//2} 0,{h}" fill="{b}"/>')
        parts.append(f'<rect x="{w - w//6}" width="{w//6}" height="{h}" fill="{c}"/>')
    body = "".join(parts)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" '
            f'width="{w}" height="{h}">{body}</svg>').encode()


MINORITIES = {
    ASHFORD: [("Ashfolk", 82.0), ("Kestrine", 11.5), ("Verric", 6.5)],
    KESTREL: [("Kestrine", 74.0), ("Ashfolk", 21.0), ("Verric", 5.0)],
    VERRICK: [("Verric", 88.0), ("Ashfolk", 12.0)],
}
MINORITY_COLORS = {"Ashfolk": [74, 127, 181], "Kestrine": [181, 86, 63], "Verric": [95, 158, 90]}


def islands():
    """The land masks. Two islands, a rock between them, and a strait."""
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)

    def blob(cx, cy, rx, ry, wobble, phase, harmonics=(3, 5, 8, 13)):
        dx = (xx - cx) / rx
        dy = (yy - cy) / ry
        ang = np.arctan2(dy, dx)
        # A plain ellipse reads as a logo, and three harmonics still reads as
        # a pebble. Four, with the high one carrying real weight, is where a
        # coastline starts having bays and headlands instead of dents.
        amp = (0.34, 0.20, 0.13, 0.07)
        r = 1.0
        for k, a in zip(harmonics, amp):
            r += wobble * a * np.sin(k * ang + phase * (0.6 + 0.37 * k))
        return (dx * dx + dy * dy) < (r * r)

    # The western island, with an inlet cut into its south-east coast. It has
    # to reach the open sea -- a cut that stops short is a lake, which is what
    # the first attempt made, and _no_inland_sea below now refuses.
    west = blob(265, 256, 82, 62, 0.24, 0.7)
    west &= ~blob(312, 296, 15, 11, 0.28, 1.9)

    # The eastern island, and a long headland reaching back toward the strait
    # so the crossing has something to aim at.
    east = blob(452, 258, 62, 50, 0.24, 2.3)
    east |= blob(412, 254, 22, 11, 0.22, 0.4)

    rock = blob(372, 262, 8, 6, 0.30, 3.1)

    masks = {WEST: west, EAST: east, ROCK: rock}
    land = west | east | rock
    _no_inland_sea(land)
    return masks, land


def _no_inland_sea(land):
    """Fail if any water is sealed off from the open sea.

    An inlet cut into a coast is a bay; the same cut a few pixels further in
    is a LAKE, and the difference is invisible in the parameters -- the first
    attempt at Ashford's harbour produced a perfectly round inland sea sitting
    in the middle of Kestrel. It also matters mechanically: the sea router
    counts water bodies, and a lake is a second one that nothing can reach.
    """
    from collections import deque
    sea = ~land
    seen = np.zeros_like(sea)
    q = deque()
    for x in range(sea.shape[1]):
        for y in (0, sea.shape[0] - 1):
            if sea[y, x] and not seen[y, x]: seen[y, x] = True; q.append((y, x))
    for y in range(sea.shape[0]):
        for x in (0, sea.shape[1] - 1):
            if sea[y, x] and not seen[y, x]: seen[y, x] = True; q.append((y, x))
    while q:
        y, x = q.popleft()
        for ny, nx in ((y+1, x), (y-1, x), (y, x+1), (y, x-1)):
            if 0 <= ny < sea.shape[0] and 0 <= nx < sea.shape[1] \
               and sea[ny, nx] and not seen[ny, nx]:
                seen[ny, nx] = True
                q.append((ny, nx))
    trapped = sea & ~seen
    if trapped.any():
        ys, xs = np.nonzero(trapped)
        raise SystemExit(
            f"the tutorial geography is wrong:\n"
            f"  {int(trapped.sum())} px of water are landlocked around "
            f"({int(xs.mean())}, {int(ys.mean())}) -- that is a lake, not a bay")


def assign():
    """Every land pixel to its nearest seed, then check the result is sane."""
    global SEEDS
    masks, land = islands()
    SEEDS = build_seeds(masks)
    sx = np.array([s[0] for s in SEEDS], np.float32)
    sy = np.array([s[1] for s in SEEDS], np.float32)

    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)

    # BORDERS THAT WANDER.
    #
    # Nearest-seed alone gives Voronoi cells, and a Voronoi border is a
    # straight line between two points -- a map of them reads as a diagram,
    # not as a country. Real borders follow ground: they bulge around a hill
    # and cut back along a river.
    #
    # The cheap way to get that is to lie about where each pixel IS before
    # asking which seed is nearest. A smooth offset field, a few harmonics
    # with no RNG in them, bends every border by the same amount its
    # neighbourhood is bent -- so the wiggle is continuous along a border
    # rather than per-pixel noise, and the two sides always agree on where
    # the line runs.
    def warp(fx, fy, phase):
        return (14.0 * np.sin(fx * 0.045 + phase) * np.cos(fy * 0.037 - phase)
                + 7.0 * np.sin(fx * 0.101 - phase * 1.7)
                + 4.0 * np.cos(fy * 0.083 + phase * 0.6))
    wx = xx + warp(xx, yy, 0.9)
    wy = yy + warp(yy, xx, 2.6)

    # NEAREST SEED **ON ITS OWN LANDMASS**.
    #
    # A plain nearest-seed over all the land lets a seed claim pixels across
    # open water: Hallow Rock, sitting in the strait, was nearest to the
    # western headland of the far island, so province 19 came out as the rock
    # PLUS a piece of Verrick -- one province in two places with sea between
    # them. On the map it looks like a bug and in play it is one: an army
    # landing on the rock is told it is standing in a province it can only
    # half reach.
    #
    # Competing only within a landmass makes that impossible rather than
    # unlikely.
    pid = np.zeros((H, W), np.int32)
    for w, mask in masks.items():
        mine = [i for i, sd in enumerate(SEEDS) if sd[3] == w]
        if not mine:
            continue
        best = np.full((H, W), np.inf, np.float32)
        here = np.zeros((H, W), np.int32)
        for i in mine:
            d = (wx - sx[i]) ** 2 + (wy - sy[i]) ** 2
            closer = d < best
            best = np.where(closer, d, best)
            here = np.where(closer, i + 1, here)
        pid = np.where(mask, here, pid)
    pid[~land] = 0

    # Any province still in more than one piece keeps the piece its seed is
    # in; the rest go to whichever neighbour they touch most. The warp can
    # pinch a cell in two around a bay, and a stray island of somebody else's
    # colour is the same defect in miniature.
    pid = _one_piece_each(pid)

    # Every seed must actually own ground, and a country's provinces must all
    # be on the island its story needs. A seed that drifts into the sea, or a
    # Kestrel province that lands on Verrick's island, breaks the script
    # rather than looking slightly wrong -- so it is an error, not a warning.
    problems = []
    for i, (x, y, cid, where) in enumerate(SEEDS, start=1):
        n = int((pid == i).sum())
        if n == 0:
            problems.append(f"province {i} ({COUNTRIES[cid]['name']}) has no land")
            continue
        if not masks[where][int(y), int(x)]:
            problems.append(f"province {i} ({COUNTRIES[cid]['name']}) is not on {where}")
        # A province the player cannot see is a province the script cannot
        # point at. The rock is meant to be small; nothing else may be.
        # 200, not the 400 this started at. The islands are much smaller now
        # -- they have to be, for a boat to cross -- and the provinces facing
        # the strait are deliberately small so their CENTRES sit near the
        # water, which is what a landing is measured against.
        if where != ROCK and n < 200:
            problems.append(f"province {i} ({COUNTRIES[cid]['name']}) is only {n} px")
    # Contiguity, checked rather than assumed: a province in two places is
    # the defect that produced Hallow Rock joined to the far island.
    from collections import deque
    for i in range(1, len(SEEDS) + 1):
        m = (pid == i)
        ys, xs = np.nonzero(m)
        if len(ys) == 0:
            continue
        seen = np.zeros(m.shape, bool)
        q = deque([(int(ys[0]), int(xs[0]))])
        seen[ys[0], xs[0]] = True
        n = 0
        while q:
            y, x = q.popleft()
            n += 1
            for ny, nx in ((y+1,x),(y-1,x),(y,x+1),(y,x-1)):
                if 0 <= ny < m.shape[0] and 0 <= nx < m.shape[1] and m[ny,nx] and not seen[ny,nx]:
                    seen[ny,nx] = True
                    q.append((ny,nx))
        if n != int(m.sum()):
            problems.append(f"province {i} is in more than one piece "
                            f"({n} of {int(m.sum())} px connected)")
    # THE CROSSING MUST BE POSSIBLE.
    #
    # The naval lesson is the point of having two islands, and a boat reaches
    # BOAT_RANGE measured to a province's CENTRE. If no province on the far
    # side is within that of water a boat can sit in, the lesson asks for
    # something the game will refuse -- which is exactly what shipped the
    # first time.
    sea = ~land
    wy, wx = np.nonzero(sea)
    def reachable_from(mask_from):
        """Province centres a boat leaving `mask_from`'s coast could land on."""
        ys, xs = np.nonzero(mask_from)
        # every sea cell within one cell of that landmass: where a boat sits
        near = []
        for bx, by in zip(wx, wy):
            if np.min((xs - bx) ** 2 + (ys - by) ** 2) <= 4:
                near.append((bx, by))
        out = set()
        for i in range(1, len(SEEDS) + 1):
            pys, pxs = np.nonzero(pid == i)
            if len(pys) == 0:
                continue
            cx, cy = pxs.mean(), pys.mean()
            for bx, by in near:
                if (cx - bx) ** 2 + (cy - by) ** 2 <= BOAT_RANGE ** 2:
                    out.add(i)
                    break
        return out
    wys2, wxs2 = np.nonzero(masks[WEST])
    eys2, exs2 = np.nonzero(masks[EAST])
    rys2, rxs2 = np.nonzero(masks[ROCK])
    def gap(a_x, a_y, b_x, b_y):
        best = 1e9
        for i in range(0, len(a_x), 7):
            d = np.min((b_x - a_x[i]) ** 2 + (b_y - a_y[i]) ** 2)
            best = min(best, d)
        return best ** 0.5
    print(f"  strait: west-rock {gap(wxs2, wys2, rxs2, rys2):.0f} cells, "
          f"rock-east {gap(rxs2, rys2, exs2, eys2):.0f}, "
          f"west-east {gap(wxs2, wys2, exs2, eys2):.0f}  "
          f"(a boat reaches {BOAT_RANGE:.0f} to a province CENTRE)")

    from_west = reachable_from(masks[WEST])
    rock_ids = {i for i, sd in enumerate(SEEDS, start=1) if sd[3] == ROCK}
    east_ids = {i for i, sd in enumerate(SEEDS, start=1) if sd[3] == EAST}
    if not (from_west & (rock_ids | east_ids)):
        problems.append(f"nothing across the water is within a boat's reach "
                        f"({BOAT_RANGE:.0f} cells) of the western island")
    from_rock = reachable_from(masks[ROCK])
    if not (from_rock & east_ids):
        problems.append("the far island is not reachable from the rock either")

    if problems:
        raise SystemExit("the tutorial geography is wrong:\n  " + "\n  ".join(problems))
    print(f"  crossing: {len(from_west & (rock_ids | east_ids))} province(s) reachable from the "
          f"west, {len(from_rock & east_ids)} from the rock")
    return masks, land, pid


def _one_piece_each(pid):
    """Leave every province as a single connected region.

    The piece that survives is the BIGGEST one, not the one holding the seed.
    The seed does not reliably lie in its own province: the borders are drawn
    on warped coordinates, so a cell can be nearest to a different seed than
    the one standing on it -- and a rule that looked for the seed's piece gave
    entire provinces away when it could not find one.
    """
    from collections import deque
    H_, W_ = pid.shape
    seen = np.zeros(pid.shape, bool)
    pieces = {}                      # province -> list of (size, cells)
    for sy in range(H_):
        for sx in range(W_):
            p = int(pid[sy, sx])
            if p == 0 or seen[sy, sx]:
                continue
            cells = []
            q = deque([(sy, sx)])
            seen[sy, sx] = True
            while q:
                y, x = q.popleft()
                cells.append((y, x))
                for ny, nx in ((y+1,x),(y-1,x),(y,x+1),(y,x-1)):
                    if 0 <= ny < H_ and 0 <= nx < W_ and pid[ny, nx] == p and not seen[ny, nx]:
                        seen[ny, nx] = True
                        q.append((ny, nx))
            pieces.setdefault(p, []).append(cells)
    for p, plist in pieces.items():
        if len(plist) < 2:
            continue
        plist.sort(key=len, reverse=True)
        for orphan in plist[1:]:
            counts = {}
            for y, x in orphan:
                for ny, nx in ((y+1,x),(y-1,x),(y,x+1),(y,x-1)):
                    if 0 <= ny < H_ and 0 <= nx < W_:
                        q2 = int(pid[ny, nx])
                        if q2 and q2 != p:
                            counts[q2] = counts.get(q2, 0) + 1
            if counts:
                winner = max(counts, key=counts.get)
                for y, x in orphan:
                    pid[y, x] = winner
    return pid


def coastal(pid, land):
    """Province ids that touch water -- the only ones that may hold a port."""
    sea = ~land
    touch = np.zeros_like(sea)
    touch[1:, :] |= sea[:-1, :]
    touch[:-1, :] |= sea[1:, :]
    touch[:, 1:] |= sea[:, :-1]
    touch[:, :-1] |= sea[:, 1:]
    return sorted({int(p) for p in np.unique(pid[touch & land]) if p})


def lonlat(x, y):
    """Coarse-grid pixel to the equirectangular lon/lat the ships use."""
    return (x / W) * 360.0 - 180.0, 90.0 - (y / H) * 180.0


def _check_the_lesson_still_tells_the_truth(by_country, armies):
    """The script quotes this map. Make it impossible to change one without the other.

    data/dialog/en/tutorial.oddlg says how many provinces Ashford holds, how
    many of Kestrel's have to be taken, and what both armies come to. Those are
    sentences a player reads and counts against the screen, and nothing in the
    build connected them to the numbers this file produces -- so a map rebuilt
    with one province more left the lesson quietly lying, in the one place a
    beginner has no way to tell it is the text that is wrong and not them. That
    is exactly what had happened: the lesson said twelve provinces and six of
    Kestrel's, on a map with seven and four.

    Cheap to check here, where both halves are in front of us.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    script = os.path.join(os.path.dirname(here), "data", "dialog", "en", "tutorial.oddlg")
    if not os.path.exists(script):
        return
    text = open(script, encoding="utf-8").read()

    def total(cid):
        return sum(u["count"] for us in armies.values() for u in us
                   if u["country_id"] == cid)

    SPELLED = {4: "four", 7: "Seven", 9: "nine",
               48_000: "forty-eight thousand",
               720_000: "seven hundred and twenty thousand"}

    def says(fragment):
        return fragment in text

    problems = []
    n_ash, n_kes = len(by_country[ASHFORD]), len(by_country[KESTREL])
    if not says("Ashford. %s provinces" % SPELLED.get(n_ash, n_ash)):
        problems.append("Ashford holds %d provinces and the opening line does not say so" % n_ash)
    if not says("all %s," % SPELLED.get(n_kes, n_kes)):
        problems.append("Kestrel holds %d provinces and the lesson tells the player "
                        "to take a different number" % n_kes)
    for cid, name in ((KESTREL, "Kestrel"), (ASHFORD, "Ashford")):
        men = total(cid)
        spelled = SPELLED.get(men)
        if spelled is None or not says(spelled):
            problems.append("%s fields %s men and the lesson does not say so "
                            "(it is quoted in words, near \"to their name\")" % (name, f"{men:,}"))
    if problems:
        raise SystemExit("the map and the lesson disagree:\n  " + "\n  ".join(problems) +
                         "\n  Fix data/dialog/en/tutorial.oddlg to match, or change this file.")


def main():
    masks, land, pid = assign()
    owner = {i: cid for i, (_, _, cid, _) in enumerate(SEEDS, start=1)}
    where = {i: w for i, (_, _, _, w) in enumerate(SEEDS, start=1)}

    # Harbours, not "every province that touches water". Nearly all of them do
    # on an island, and a map where everything is a port teaches nothing about
    # choosing one. The biggest coastline on each landmass per country is the
    # major port; one more each is a minor.
    touching = set(coastal(pid, land))
    ports = {}
    for cid in COUNTRIES:
        for w in (WEST, EAST, ROCK):
            here = [p for p in touching if owner[p] == cid and where[p] == w]
            if not here:
                continue
            here.sort(key=lambda p: -int((pid == p).sum()))
            for rank, p in enumerate(here[:2]):
                ports[p] = 3 if rank == 0 and w != ROCK else 1

    # A HARBOUR ON THE STRAIT, whoever owns it.
    #
    # The biggest-coastline rule puts Ashford's ports on the far side of their
    # own island, because that is where their big provinces are -- so the
    # crossing began with a long sail the wrong way round. The province on the
    # western island nearest the water Verrick is across gets one regardless
    # of size. It is Kestrel's at the start and the player's by the time they
    # need it, which is the whole shape of the lesson.
    strait_x = SEEDS[[i for i, sd in enumerate(SEEDS) if sd[3] == ROCK][0]][0]
    west_coastal = [p for p in touching if where[p] in (WEST,)]
    def dist_to_strait(p):
        xs = np.nonzero(pid == p)[1]
        return abs(float(xs.max()) - strait_x)
    near = min(west_coastal, key=dist_to_strait)
    ports[near] = max(ports.get(near, 0), 2)
    provs = list(owner)
    by_country = {c: [p for p in provs if owner[p] == c] for c in COUNTRIES}

    # ── layers ──
    full_pid = np.repeat(np.repeat(pid, K, axis=0), K, axis=1)
    prov_rgb = np.zeros((FULL_H, FULL_W, 3), np.uint8)
    prov_rgb[..., 0] = (full_pid >> 16) & 0xFF
    prov_rgb[..., 1] = (full_pid >> 8) & 0xFF
    prov_rgb[..., 2] = full_pid & 0xFF

    full_land = np.repeat(np.repeat(land, K, axis=0), K, axis=1)
    ls = np.zeros((FULL_H, FULL_W, 4), np.uint8)
    ls[full_land] = (255, 255, 255, 255)          # land: opaque. Sea: clear.

    # ── the data ──
    provinces = {str(p): {"id": p, "name": "", "country_id": owner[p],
                          "iso_a3": COUNTRIES[owner[p]]["iso"],
                          "color": "#%06x" % p} for p in provs}

    countries = {}
    for cid, m in COUNTRIES.items():
        countries[str(cid)] = {
            "id": cid, "iso_a3": m["iso"], "name": m["name"], "color": m["color"],
            # The image AND the colours. The picture is the SVG; the colour
            # list is what anything deriving a NEW flag reads -- a breakaway
            # state takes its parent's colours, and a parent with none gets a
            # randomly generated one that looks unrelated to the country it
            # just broke away from.
            "flag_actual": {"type": "hstripes_3", "colors": m["flag"],
                            "image": "flags/" + m["iso"] + ".svg"},
            # The censored variant is the same three colours as plain bands,
            # so a player with that setting on still gets a flag rather than a
            # blank -- see audit_flag_licenses.py for why the game has two.
            "flag_censored": {"type": "hstripes_3", "colors": m["flag"], "censored": True},
            "treasury": m["treasury"],
        }

    population, resources, compass, minorities = {}, {}, {}, {}
    for p in provs:
        cid = owner[p]
        area = int((pid == p).sum())
        # Population follows area, so a big province is worth more than a
        # small one and the map reads the way a player expects.
        pop = 120_000 + area * 320
        population[str(p)] = pop
        industry = 2 if cid == ASHFORD else 1
        resources[str(p)] = {
            "oil": {"a": 0.0, "b": 0.0}, "gold": {"a": 0.0, "b": 0.0},
            "rubber": {"a": 0.0, "b": 0.0}, "gemstones": {"a": 0.0, "b": 0.0},
            "metal": {"a": 4.0 if cid == KESTREL else 1.5, "b": 1.0},
            "industry": {"level": industry, "income": 0.6 * industry,
                         "specialization": "Metal", "resourceIncome": 0.15,
                         "popIncome": pop / 4_000_000.0, "popModifier": 1.0,
                         "fortification": 0},
            "fortification": 1,
        }
        compass[str(p)] = dict(COUNTRIES[cid]["compass"])
        minorities[str(p)] = [{"n": n, "p": pc} for n, pc in MINORITIES[cid]]

    # Armies sit where the fighting will be, not spread evenly: nearest the
    # OTHER country's ground. A tutorial about moving an army needs the army
    # to already be somewhere that makes moving it obvious.
    centres = {p: (float(np.nonzero(pid == p)[1].mean()),
                   float(np.nonzero(pid == p)[0].mean())) for p in provs}
    # ASHFORD IS DELIBERATELY OVERWHELMING against Kestrel.
    #
    # The land lesson is "give every order, then end the turn, and watch them
    # all happen at once" -- which only lands if the whole of Kestrel can
    # actually fall in that one turn. A fair fight teaches the player that
    # orders are slow and armies grind, which is true of the real game and
    # exactly the wrong first thing to learn about the button.
    #
    # Verrick is not weakened: crossing water is the lesson there, and an
    # opponent who folds to the first landing does not teach it.
    armies = {}
    for cid, plist in by_country.items():
        strength = {ASHFORD: 150_000, KESTREL: 18_000, VERRICK: 60_000}[cid]
        foreign = [centres[p] for p in provs if owner[p] != cid]
        def nearest_enemy(p):
            x, y = centres[p]
            return min((x - fx) ** 2 + (y - fy) ** 2 for fx, fy in foreign)
        # Ashford's men are spread across MORE provinces than the others, so
        # every one of Kestrel's has a neighbour to be taken from.
        spread = 6 if cid == ASHFORD else 4
        step = 12_000 if cid == ASHFORD else 4_000
        for i, p in enumerate(sorted(plist, key=nearest_enemy)[:spread]):
            armies[str(p)] = [{"country_id": cid, "count": strength - i * step}]

    # SHIPS GO IN THE WATER.
    #
    # They were placed at the CENTROID of a port province, which is the middle
    # of an island -- so every one of them started aground and the game had to
    # refloat them, dumping the fleet wherever its search happened to end.
    # A port province is a piece of land that touches the sea; the ship wants
    # the sea part, a little way off its own coast.
    sea = ~land
    ships = []
    for cid in (ASHFORD, VERRICK):
        for i, p in enumerate([q for q in by_country[cid] if q in ports][:3]):
            ys, xs = np.nonzero(pid == p)
            cx, cy = float(xs.mean()), float(ys.mean())
            # The nearest open water to this province's middle, then a couple
            # of cells further out so the hull is clear of the beach.
            wy, wx = np.nonzero(sea)
            d2 = (wx - cx) ** 2 + (wy - cy) ** 2
            k = int(np.argmin(d2))
            ox, oy = float(wx[k]), float(wy[k])
            step = np.hypot(ox - cx, oy - cy)
            if step > 0.5:
                ox += (ox - cx) / step * 3.0
                oy += (oy - cy) / step * 3.0
            ox = float(np.clip(ox, 0, W - 1))
            oy = float(np.clip(oy, 0, H - 1))
            if land[int(round(oy)), int(round(ox))]:
                raise SystemExit(f"ship for province {p} still on land at ({ox:.0f},{oy:.0f})")
            lon, lat = lonlat(ox, oy)
            ships.append({"country_id": cid, "type": "boat" if i else "destroyer",
                          "lon": round(lon, 4), "lat": round(lat, 4),
                          "health": 100, "crew": 300})

    flags = {"flags/" + m["iso"] + ".svg": flag_svg(m["banner"], m["flag"])
             for m in COUNTRIES.values()}
    donor = __import__("zipfile").ZipFile(DONOR)
    members = {
        "land_sea.png": layer_png(ls),
        "provinces.png": layer_png(prov_rgb),
        # A placeholder. The real one is drawn below by the same tool that
        # keeps every other map's preview honest -- hand-rolling it here would
        # be a second renderer to disagree with the first, which is exactly
        # what tests/run_all.sh's preview check exists to catch.
        "thumb.png": _png_bytes(Image.new("RGB", (512, 256), (16, 22, 34))),
        "provinces.json": _j(provinces),
        "countries.json": _j(countries),
        "population.json": _j(population),
        "resources.json": _j(resources),
        "minorities.json": _j(minorities),
        "minority_colors.json": _j(MINORITY_COLORS),
        "political_compass.json": _j(compass),
        "country_compass.json": _j({m["iso"]: m["compass"] for m in COUNTRIES.values()}),
        "starting_policies.json": _j({"starting_policies": {
            "ASH": ["free_press", "public_schooling"],
            "KES": ["conscription", "censorship"],
            "VER": ["state_industry", "land_reform"],
        }}),
        "starting_minority_policies.json": _j({}),
        # Kestrel is already hostile, so the land lesson has a reason to
        # happen. Verrick is not: the player has to decide to cross.
        "relations.json": _j({"ASH": {"KES": {"war": True}},
                              "KES": {"ASH": {"war": True}}}),
        "claims.json": _j({"ASH": by_country[KESTREL][:3],
                           "KES": by_country[ASHFORD][:2]}),
        "ports.json": _j({str(p): {"level": lv} for p, lv in sorted(ports.items())}),
        "armies.json": _j(armies),
        "ships.json": _j(ships),
        # The policy CATALOGUE is global game content, not geography. Taken
        # from the shipped map so the tutorial teaches the same policies the
        # player will meet in a real game.
        "policies.json": donor.read("policies.json"),
        "metadata.json": _j({
            "name": "Tutorial",
            "description": "Two islands, three small countries, and nothing at stake. "
                           "Built for the tutorial and not saved.",
            "author": "OpenDoctrines",
            "map_date": "Spring 1936 AD",
            "license": "CC-BY-4.0",
            "has_scripts": False,
        }),
    }

    _check_the_lesson_still_tells_the_truth(by_country, armies)

    members.update(flags)
    write_odmap(OUT, members)

    # Redraw the browser preview from the map that was just written.
    import subprocess
    subprocess.run([sys.executable,
                    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "rebuild_map_preview.py"),
                    "--map", "tutorial.odmap"],
                   check=True)

    print(f"wrote {OUT}")
    print(f"  {len(provs)} provinces, {len(COUNTRIES)} countries, "
          f"{len(ports)} harbours ({sum(1 for v in ports.values() if v == 3)} major)")
    for cid, m in COUNTRIES.items():
        print(f"    {m['name']:9} {len(by_country[cid]):2} provinces  "
              f"ids {by_country[cid][0]}..{by_country[cid][-1]}")


def _j(obj):
    return json.dumps(obj, separators=(",", ":")).encode()


def _png_bytes(img):
    import io
    b = io.BytesIO()
    img.save(b, "PNG")
    return b.getvalue()


if __name__ == "__main__":
    main()
