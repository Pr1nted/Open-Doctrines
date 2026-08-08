#!/usr/bin/env python3
"""
Where a hull may float, and what kind of hull it may be.

Imported by tools/generate_scenario.py (which builds the historical maps) and
tools/fix_naval_layer.py (which repairs the ones already shipped). Both must
agree, or the next regeneration silently undoes the repair -- which is exactly
how the shipped maps ended up carrying hull types the generator had stopped
being allowed to emit.

WHY SHIPS LOOKED LIKE THEY WERE ON LAND

They were not, quite. Every hull in every historical map sat in water by a
single-pixel test -- and 98% of them sat within four pixels of the shore, 167
of the 170 on The Powder Keg, against 13% on the Modern Day map.

The cause was the berth search. It walked from the port province's centroid to
the NEAREST water pixel and nudged four pixels further out, so every hull ended
up in the first estuary, fjord or river mouth it found, pressed against the
coast. A hull icon is about twelve screen pixels across; four pixels of
clearance means most of it is drawn over land. The ship is in the sea and looks
like it is in a field.

So the rule here is clearance, not adjacency: a berth must have open water
around it in every direction, and the search prefers more clearance over less
rather than taking the first water it touches.

WHAT COUNTS AS A HULL

Only what the game can both draw and price. Game_Render.cpp gives boat,
destroyer and carrier their own shapes; Game_Economy.cpp charges upkeep for
carriers and destroyers; Game_Render.cpp lets a port build destroyers at level
2 and carriers at level 3. Battleships and cruisers are in neither list. They
existed only in map files, so a sunk one was gone for good, and for most of
their life they had no sprite and dealt no damage. The engine now has fallbacks
for both, but a fallback is insurance against a mod's own hull -- it is not a
licence for the shipped maps to keep using types the player can never replace.
"""

import numpy as np

# Roughly the modern map's own mix (73 destroyers, 93 boats, 8 carriers), as a
# repeating pattern rather than a random draw so that regenerating a map twice
# produces the same fleet.
HULL_CYCLE = ["destroyer", "boat", "destroyer", "boat", "boat",
              "destroyer", "boat", "destroyer", "boat", "carrier"]

# A hull icon is ~12 screen pixels across at the zoom the navy view opens at,
# so six map pixels of water in every direction is the point at which the whole
# icon is over sea. Below this it reads as beached however correct the
# underlying coordinate is.
WANT_CLEARANCE = 6
MIN_CLEARANCE = 3          # never berth tighter than this if any alternative exists
BERTH_SEARCH = 160         # px from the port before a hull is given up on
HULL_SPACING = 10          # px between hulls, so a fleet reads as a fleet


def hull_type(index):
    """Deterministic hull for the index-th ship of a fleet."""
    return HULL_CYCLE[index % len(HULL_CYCLE)]


def clearance_field(land, cap=16):
    """Pixels of open water in the tightest direction, per water pixel.

    Successive erosion rather than a distance transform: a pixel that survives
    k erosions of the water mask has at least k pixels of water on every side,
    which is precisely the question a hull icon asks. Land is 0.

    THE STRUCTURING ELEMENT IS THE FULL 3x3, NOT THE FOUR CARDINALS.

    Eroding by the 4-neighbourhood measures Manhattan distance, and a hull icon
    is not a diamond -- it is a square, drawn axis-aligned. A pixel three
    across and three down from a headland has a Manhattan clearance of six and
    a headland inside its own icon. Measured that way the first run of this
    re-berthing reported every fleet clear at six pixels and then left 53 of
    them still overlapping the shore, which is the bug it was written to fix,
    surviving the fix by arriving in a different metric.

    The 3x3 element measures Chebyshev distance, which is the side of the
    largest all-water square centred on the pixel -- the same shape as the
    thing being drawn.

    A result of k means exactly: no land within k pixels in any direction, and
    land somewhere at k+1. So a hull needing a clear icon of half-width n wants
    a berth scoring at least n.

    Columns wrap -- the Pacific is one ocean and a berth near the antimeridian
    is not against a wall.
    """
    clear = np.zeros(land.shape, dtype=np.uint8)
    m = ~land
    for _ in range(cap):
        e = m.copy()
        e[1:, :] &= m[:-1, :]
        e[:-1, :] &= m[1:, :]
        for dy in (-1, 0, 1):
            row = m if dy == 0 else np.roll(m, dy, axis=0)
            e &= np.roll(row, 1, axis=1)
            e &= np.roll(row, -1, axis=1)
        e[0, :] = False          # poles are a wall, not a wrap
        e[-1, :] = False
        m = e
        if not m.any():
            break
        clear[m] += 1
    return clear


def coastal_anchor(prov_mask, big_water, centroid):
    """The province's own pixel that touches open sea, nearest its centroid.

    This is where a harbour is, and it is not where the centroid is. A province
    centroid is a point in the middle of a landmass -- or, for a province that
    curves around a bay, a point in the middle of the bay. Thirty-odd port
    anchors per map were being drawn floating in open water for exactly that
    reason, and a handful landed inside a neighbouring country.
    """
    touch = np.zeros_like(prov_mask)
    touch[1:, :] |= big_water[:-1, :]
    touch[:-1, :] |= big_water[1:, :]
    touch[:, 1:] |= big_water[:, :-1]
    touch[:, :-1] |= big_water[:, 1:]
    rim = prov_mask & touch
    if not rim.any():
        # No coast at all: an island smaller than one raster cell, whose every
        # pixel the coastline calls water. The centroid is the only honest
        # answer and it is already in the sea, where a harbour belongs.
        return centroid
    ys, xs = np.nonzero(rim)
    cx, cy = centroid
    i = int(np.argmin((xs - cx) ** 2 + (ys - cy) ** 2))
    return int(xs[i]), int(ys[i])


def find_berth(clear, ox, oy, taken, want=WANT_CLEARANCE,
               floor=MIN_CLEARANCE, radius=BERTH_SEARCH, spacing=HULL_SPACING):
    """Nearest berth to (ox, oy) with real water around it.

    Returns (x, y, clearance) or None. Rings outward and keeps the best
    candidate seen so far; a berth with the wanted clearance ends the search
    immediately, otherwise the roomiest thing within `radius` wins. That
    ordering matters: taking the first pixel that merely clears `floor` is how
    a fleet ends up strung along a coastline again, one notch out.
    """
    h, w = clear.shape
    best = None
    for r in range(1, radius):
        if best is not None and best[2] >= want:
            break
        y0, y1 = max(0, oy - r), min(h, oy + r + 1)
        if y0 >= y1:
            continue
        # perimeter of the square at radius r, columns wrapped
        xs = [(ox + dx) % w for dx in (-r, r)]
        cand = []
        for x in xs:
            col = clear[y0:y1, x]
            for k in np.flatnonzero(col >= floor):
                cand.append((x, y0 + int(k)))
        for y in (oy - r, oy + r):
            if 0 <= y < h:
                idx = [(ox + dx) % w for dx in range(-r, r + 1)]
                row = clear[y, idx]
                for k in np.flatnonzero(row >= floor):
                    cand.append((idx[int(k)], y))
        for x, y in cand:
            c = int(clear[y, x])
            if any(abs(x - tx) < spacing and abs(y - ty) < spacing
                   for tx, ty in taken):
                continue
            if best is None or c > best[2]:
                best = (x, y, c)
                if c >= want:
                    break
    return best


def pixel_to_lonlat(x, y, w, h):
    """Pixel CENTRE, not corner.

    Converting a corner back to a pixel index elsewhere rounds down on a float
    hair, and one pixel inland from a coastline is land. That alone used to put
    a fifth of the fleet on the beach.
    """
    return ((x + 0.5) / w * 360.0 - 180.0,
            90.0 - (y + 0.5) / h * 180.0)
