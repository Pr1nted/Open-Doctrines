#!/usr/bin/env python3
"""Turn an .odmap into something a 16-colour machine with no decoder can draw.

TempleOS cannot read a PNG, has 640x480 in sixteen fixed colours, and would not
thank you for 460 KB of province raster. So the chewing happens here:

  * every pixel is resolved to the colour of whoever owns it,
  * that colour is snapped to the nearest of the sixteen it actually has,
  * the result is run-length encoded, which a political map compresses
    enormously because it is broad flat regions by construction,
  * province centres come along so the guest can answer a mouse click without
    carrying a per-pixel index it has no room for.

The output is a few tens of KB and decodes with a memset per run.

    python3 tools/templeos_map.py data/STDmaps/map.odmap templeos/world.odt
"""
from __future__ import annotations

import argparse
import json
import pathlib
import struct
import sys
import zipfile

# ── THE SIXTEEN, FROM THE MACHINE ITSELF ──
#
# Adam/Gr/GrPalette.HC, gr_palette_std. Stored as CBGR48 with red in the top
# 16 bits of each entry; these are the top byte of each channel. Copied rather
# than approximated, because "close enough to EGA" is how you end up with a map
# whose colours are all one step off and nobody can say why.
PALETTE = [
    (0x00, 0x00, 0x00), (0x00, 0x00, 0xAA), (0x00, 0xAA, 0x00), (0x00, 0xAA, 0xAA),
    (0xAA, 0x00, 0x00), (0xAA, 0x00, 0xAA), (0xAA, 0x55, 0x00), (0xAA, 0xAA, 0xAA),
    (0x55, 0x55, 0x55), (0x55, 0x55, 0xFF), (0x55, 0xFF, 0x55), (0x55, 0xFF, 0xFF),
    (0xFF, 0x55, 0x55), (0xFF, 0x55, 0xFF), (0xFF, 0xFF, 0x55), (0xFF, 0xFF, 0xFF),
]
SEA, UNCLAIMED, BORDER = 1, 8, 0   # BLUE, DKGRAY, BLACK
# Black is the border ink and blue is the sea, so neither may be given to a
# country. That leaves fourteen for 180-odd countries, which is why they are
# assigned by adjacency below rather than by hue.
LAND = [i for i in range(16) if i not in (SEA, BORDER)]

MAGIC = b"ODTM"
VERSION = 2


def nearest(rgb, exclude=()):
    """The palette index closest to rgb, in plain squared distance.

    `exclude` keeps land off the sea's colour. Two countries landing on the
    same index is expected and fine -- there are 180-odd of them and sixteen
    colours, so the map reads by shape and border, not by hue.
    """
    r, g, b = rgb
    best, best_d = 0, None
    for i, (pr, pg, pb) in enumerate(PALETTE):
        if i in exclude:
            continue
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if best_d is None or d < best_d:
            best, best_d = i, d
    return best


def colour_countries(cid_at, W, H, want):
    """Give neighbouring countries different colours.

    Snapping each country to its nearest palette entry is the obvious thing and
    it produces an unreadable map: 180 countries land on about seven colours,
    and most borders end up between two identical ones. So the constraint that
    actually matters -- *neighbours must differ* -- is solved directly, the way
    a printed atlas does it. Four colours provably suffice for a planar map;
    there are fourteen here, so a greedy pass in descending degree never runs
    out in practice, and preferring each country's true hue among the free ones
    keeps Brazil greenish and Russia reddish where nothing objects.
    """
    adj: dict[int, set[int]] = {}
    for y in range(H):
        row = y * W
        for x in range(W):
            a = cid_at[row + x]
            if a is None:
                continue
            for nx, ny in ((x + 1, y), (x, y + 1)):
                if nx >= W or ny >= H:
                    continue
                b = cid_at[ny * W + nx]
                if b is not None and b != a:
                    adj.setdefault(a, set()).add(b)
                    adj.setdefault(b, set()).add(a)

    out: dict[int, int] = {}
    for cid in sorted(adj, key=lambda c: -len(adj[c])):
        taken = {out[n] for n in adj[cid] if n in out}
        free = [c for c in LAND if c not in taken] or LAND
        out[cid] = min(free, key=lambda c: abs(c - want.get(cid, UNCLAIMED)))
    for cid in want:
        out.setdefault(cid, want[cid])
    return out


def rle(indices: bytes) -> bytearray:
    """(count, colour) pairs, counts capped at 255.

    Row-major over the whole image rather than per row: a run that crosses the
    right edge into the next row costs nothing to decode and saves a pair on
    every line of open ocean.
    """
    out = bytearray()
    if not indices:
        return out
    cur, n = indices[0], 1
    for v in indices[1:]:
        if v == cur and n < 255:
            n += 1
        else:
            out += bytes((n, cur))
            cur, n = v, 1
    out += bytes((n, cur))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("odmap")
    ap.add_argument("out")
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=400,
                    help="640x480 is the screen; the rest is the panel")
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

    # Both files are dicts keyed by the id as a string. The province's owner is
    # `country_id` -- spelled that way, and a lookup that misses it silently
    # paints the entire world unclaimed, which is exactly what the first run of
    # this did.
    owner = {int(k): int(v["country_id"]) for k, v in provinces.items()
             if "country_id" in v}
    colour = {}
    for k, c in countries.items():
        col = c.get("color", "")
        if isinstance(col, str) and col.startswith("#") and len(col) >= 7:
            rgb = tuple(int(col[i:i + 2], 16) for i in (1, 3, 5))
            colour[int(k)] = nearest(rgb, exclude={SEA})
    if not colour or not owner:
        print("owner/colour tables came out empty -- the map format has moved",
              file=sys.stderr)
        return 1

    W, H = a.width, a.height
    sw, sh = src.size
    px = src.load()

    # Nearest-neighbour on province IDS, never on colours: averaging a province
    # raster invents ids that do not exist.
    cid_at: list = [None] * (W * H)
    pid_at = [0] * (W * H)
    centre_acc: dict[int, list[int]] = {}
    for y in range(H):
        syy = y * sh // H
        for x in range(W):
            r, g, b = px[x * sw // W, syy]
            pid = (r << 16) | (g << 8) | b
            if pid == 0:
                continue
            cid = owner.get(pid, -1)
            k = y * W + x
            cid_at[k], pid_at[k] = cid, pid
            acc = centre_acc.setdefault(pid, [0, 0, 0, cid])
            acc[0] += x; acc[1] += y; acc[2] += 1

    assigned = colour_countries(cid_at, W, H, colour)

    indices = bytearray(W * H)
    for k in range(W * H):
        cid = cid_at[k]
        indices[k] = SEA if cid is None else assigned.get(cid, UNCLAIMED)

    # ── BORDERS, INKED ──
    #
    # Two countries that happen to share a colour still read as two countries
    # if there is a line between them, and at this size a line is the only
    # thing carrying the shape of Europe at all. Only country borders: province
    # borders at 640x400 are noise, and coastlines already have the sea.
    for y in range(H):
        for x in range(W):
            k = y * W + x
            c = cid_at[k]
            if c is None:
                continue
            for nx, ny in ((x + 1, y), (x, y + 1)):
                if nx < W and ny < H:
                    n = cid_at[ny * W + nx]
                    if n is not None and n != c:
                        indices[k] = BORDER
                        break

    packed = rle(bytes(indices))
    centres = [(pid, v[0] // v[2], v[1] // v[2], v[3]) for pid, v in centre_acc.items()
               if v[2] >= 4]          # a province of three pixels cannot be clicked
    centres.sort()

    # ── THE ISO TABLE ──
    #
    # The map knows owners as numbers; the agent door only ever says "Sweden
    # (SWE)". Without a join between them the guest cannot tell which provinces
    # are its own, so the codes ride along -- four bytes each, 180-odd of them.
    iso = {}
    for k, c in countries.items():
        code = str(c.get("iso_a3", ""))[:3]
        if code:
            iso[int(k)] = code
    seen = sorted({cid for _, _, _, cid in centres if cid in iso})

    blob = bytearray()
    blob += MAGIC
    blob += struct.pack("<HHHHHI", VERSION, W, H, len(centres), len(seen), len(packed))
    blob += packed
    for pid, cx, cy, cid in centres:
        # UNSIGNED: the sentinel owners (unclaimed is 65533, blocked 65534)
        # do not fit a signed short, and packing them as one throws.
        blob += struct.pack("<IHHH", pid, cx, cy, cid & 0xFFFF)

    for cid in seen:
        blob += struct.pack("<H", cid & 0xFFFF) + iso[cid].encode("ascii").ljust(4, b"\0")

    pathlib.Path(a.out).write_bytes(blob)
    raw = W * H
    print(f"{a.out}: {W}x{H}, {len(packed) // 2} runs, {len(centres)} provinces, "
          f"{len(seen)} countries, "
          f"{len(blob) / 1024:.1f} KB (raster {raw / 1024:.0f} KB raw, "
          f"{100 * len(packed) / raw:.0f}%)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
