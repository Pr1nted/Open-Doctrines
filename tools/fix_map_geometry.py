#!/usr/bin/env python3
"""Repair province geometry inside shipped .odmap archives, in place.

WHAT IS WRONG WITH THE SHIPPED GEOMETRY

Two defects, both from the generator's step 6.5 (now fixed upstream in
tools/map_generator/Generator.cpp, which only helps maps generated from here on):

  MINUSCULE PROVINCES. The floor was 80 pixels on an 8,192 x 4,096 raster and
  did not hold: Modern Day ships nine provinces under it and a smallest of TWO
  (Norway #830 is four). Across the six shipped maps there are 84-89 provinces
  below a sane floor each. "Some absolutely minuscule provinces exist."

  PROVINCES IN TWO PLACES. The merge folded fragments into whatever it found
  within a two-hundred-pixel search, touching or not, so provinces ended up with
  detached lumps on the far side of somebody else. 415-442 of each map's
  provinces are in more than one piece; about twenty per map have a detached
  piece big enough to be a place in its own right.

WHAT THIS DOES

  1. A province below MIN_PROVINCE is absorbed by a neighbour it TOUCHES,
     preferring one in the same country. Its people, industry, port and
     minorities go with it.
  2. A detached piece of at least MIN_PIECE becomes a province of its own,
     taking a share of its parent's population and economy proportional to its
     area, so nothing is created out of nothing.
  3. Smaller detached fragments are left where they are. They are archipelago
     pixels, they belong to the province that owns the archipelago, and the
     renderer now anchors markers on a province's largest piece anyway.

Every province-keyed data file in the archive is rewritten to match:
provinces, population, political_compass, minorities, resources, ports, armies,
and the province lists inside claims.

  python3 tools/fix_map_geometry.py data/STDmaps/*.odmap
  python3 tools/fix_map_geometry.py --check data/STDmaps/*.odmap

PROVINCE IDS CHANGE, so every .odsv written against these maps is invalidated.
See AGENTS.md; regenerate data/saves after running this.
"""
import io
import json
import os
import shutil
import sys
import zipfile
from collections import deque, defaultdict

MIN_PROVINCE = 250      # a province smaller than this is not a place
MIN_PIECE = 250         # a detached piece this big deserves its own id

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("needs numpy and pillow: python3 -m pip install numpy pillow")
    sys.exit(2)

PROV_KEYED = ["population.json", "political_compass.json", "minorities.json",
              "resources.json", "ports.json", "armies.json"]


def to_id(r, g, b):
    return (int(r) << 16) | (int(g) << 8) | int(b)


def to_rgb(pid):
    return ((pid >> 16) & 0xFF, (pid >> 8) & 0xFF, pid & 0xFF)


def components(pid_img):
    """{province id: [piece, ...]} with pieces sorted largest first.

    Eight-connected, and the world wraps in x -- a province at the antimeridian
    is one piece, not two.
    """
    H, W = pid_img.shape
    flat = pid_img.ravel()
    order = np.argsort(flat, kind="stable")
    sv = flat[order]
    keys = sorted(set(int(x) for x in np.unique(flat)) - {0})
    starts = np.searchsorted(sv, keys, "left")
    ends = np.searchsorted(sv, keys, "right")
    out = {}
    for k, a, b in zip(keys, starts, ends):
        idxs = set(order[a:b].tolist())
        comps = []
        while idxs:
            seed = next(iter(idxs))
            idxs.discard(seed)
            q = deque([seed])
            piece = [seed]
            while q:
                cur = q.popleft()
                y, x = divmod(cur, W)
                for dy in (-1, 0, 1):
                    ny = y + dy
                    if ny < 0 or ny >= H:
                        continue
                    for dx in (-1, 0, 1):
                        if dx == 0 and dy == 0:
                            continue
                        n = ny * W + (x + dx) % W
                        if n in idxs:
                            idxs.discard(n)
                            q.append(n)
                            piece.append(n)
            comps.append(piece)
        comps.sort(key=len, reverse=True)
        out[k] = comps
    return out


def touching(pid_img, pixels, own):
    """Provinces this set of pixels physically touches, by contact length."""
    H, W = pid_img.shape
    votes = defaultdict(int)
    for idx in pixels:
        y, x = divmod(idx, W)
        for dy in (-2, -1, 0, 1, 2):
            ny = y + dy
            if ny < 0 or ny >= H:
                continue
            for dx in (-2, -1, 0, 1, 2):
                n = pid_img[ny, (x + dx) % W]
                if n and n != own:
                    votes[int(n)] += 1
    return votes


# ── Merging and splitting the DATA, not just the raster ──────────────────
#
# The rules follow generate_scenario.py's re-cut, which had to answer the same
# question when it merged provinces into scenario countries: population sums,
# industry level takes the best of the group rather than diluting it, a port
# keeps the highest level, resources accumulate at half weight (two oil
# provinces merged are richer than one, not twice as rich), and composition is
# averaged by population.
#
# Splitting is the same rules read backwards, with one difference that matters:
# anything that is a QUANTITY is divided by area share, so a province cut in two
# does not become two provinces each as rich as the original. Anything that is a
# PROPERTY -- industry level, port level, ideology, ethnic composition -- is
# inherited whole, because half a province is still as industrialised as it was.

def merge_data(data, src, dst):
    pop = data["population.json"]
    sp = pop.pop(str(src), 0)
    pop[str(dst)] = pop.get(str(dst), 0) + sp

    comp = data["political_compass.json"]
    a, b = comp.pop(str(src), None), comp.get(str(dst))
    if a and b:
        # Population-weighted, because a two-pixel province should not drag a
        # county's politics anywhere.
        w = sp / max(1.0, sp + pop.get(str(dst), 0))
        comp[str(dst)] = {"left": round(b["left"] * (1 - w) + a["left"] * w),
                          "auth": round(b["auth"] * (1 - w) + a["auth"] * w)}
    elif a and not b:
        comp[str(dst)] = a

    mino = data["minorities.json"]
    ma, mb = mino.pop(str(src), None), mino.get(str(dst))
    if ma and mb:
        acc = defaultdict(float)
        total = max(1.0, sp + pop.get(str(dst), 0))
        for g in mb:
            acc[g["n"]] += g["p"] * (1 - sp / total)
        for g in ma:
            acc[g["n"]] += g["p"] * (sp / total)
        mino[str(dst)] = [{"n": n, "p": round(p, 1)} for n, p in
                          sorted(acc.items(), key=lambda kv: -kv[1]) if p >= 0.05]
    elif ma and not mb:
        mino[str(dst)] = ma

    res = data["resources.json"]
    ra, rb = res.pop(str(src), None), res.get(str(dst))
    if ra and rb:
        for key in ("oil", "gold", "rubber", "gemstones", "metal"):
            if key in ra and key in rb:
                for f in ("a", "b"):
                    rb[key][f] = round(rb[key].get(f, 0) + ra[key].get(f, 0) * 0.5, 3)
        ia, ib = ra.get("industry"), rb.get("industry")
        if ia and ib:
            ib["level"] = max(ib.get("level", 0), ia.get("level", 0))
            for f in ("income", "resourceIncome", "popIncome"):
                ib[f] = round(ib.get(f, 0) + ia.get(f, 0), 3)
            ib["fortification"] = max(ib.get("fortification", 0), ia.get("fortification", 0))
    elif ra and not rb:
        res[str(dst)] = ra

    ports = data["ports.json"]
    pa = ports.pop(str(src), None)
    if pa:
        cur = ports.get(str(dst))
        ports[str(dst)] = {"level": max(cur["level"], pa["level"])} if cur else pa

    armies = data["armies.json"]
    aa = armies.pop(str(src), None)
    if aa:
        cur = armies.setdefault(str(dst), [])
        for u in aa:
            for e in cur:
                if e["country_id"] == u["country_id"]:
                    e["count"] += u["count"]
                    break
            else:
                cur.append(u)


def split_data(data, src, new, share):
    """Give province `new` a `share` (0..1) of `src`'s quantities."""
    pop = data["population.json"]
    sp = pop.get(str(src), 0)
    moved = int(round(sp * share))
    pop[str(src)] = sp - moved
    pop[str(new)] = moved

    for f in ("political_compass.json", "minorities.json"):
        v = data[f].get(str(src))
        if v is not None:
            data[f][str(new)] = json.loads(json.dumps(v))   # a property, inherited whole

    res = data["resources.json"]
    r = res.get(str(src))
    if r is not None:
        cp = json.loads(json.dumps(r))
        for key in ("oil", "gold", "rubber", "gemstones", "metal"):
            if key in r:
                for f in ("a", "b"):
                    keep = round(r[key].get(f, 0) * (1 - share), 3)
                    cp[key][f] = round(r[key].get(f, 0) - keep, 3)
                    r[key][f] = keep
        ia = r.get("industry")
        if ia:
            # Level and specialisation are what the place IS; the money it makes
            # is how much of it there is.
            for f in ("income", "resourceIncome", "popIncome"):
                keep = round(ia.get(f, 0) * (1 - share), 3)
                cp["industry"][f] = round(ia.get(f, 0) - keep, 3)
                ia[f] = keep
        res[str(new)] = cp

    # A port and a garrison are in ONE place. They stay with the piece that has
    # the larger share; nothing is duplicated.
    if share > 0.5:
        for f in ("ports.json", "armies.json"):
            v = data[f].pop(str(src), None)
            if v is not None:
                data[f][str(new)] = v


def process(path, check_only=False):
    z = zipfile.ZipFile(path)
    members = z.infolist()
    raw = {i.filename: z.read(i.filename) for i in members}
    z.close()

    img = np.array(Image.open(io.BytesIO(raw["provinces.png"])).convert("RGB"), dtype=np.int32)
    pid_img = (img[:, :, 0] << 16) | (img[:, :, 1] << 8) | img[:, :, 2]
    H, W = pid_img.shape

    data = {f: json.loads(raw[f]) for f in PROV_KEYED + ["provinces.json", "claims.json"]
            if f in raw}
    for f in PROV_KEYED:
        data.setdefault(f, {})
    provs = data["provinces.json"]

    comps = components(pid_img)
    next_id = max(comps) if comps else 0
    merged = split = 0

    # ── 1. Provinces too small to be places ──────────────────────────────
    #
    # Smallest first, so a chain of specks collapses into the big neighbour
    # rather than into each other.
    #
    # RESOLVED THROUGH A REMAP, not painted as we go. Merges chain: a speck goes
    # into a small neighbour which is itself under the floor and goes into a
    # third. Painting immediately and then reading the NEXT merge's pixels from
    # the component list computed before any of it left the first speck's pixels
    # carrying an id that had since been deleted -- a province in the raster
    # that provinces.json had never heard of. The remap is applied to the whole
    # raster in one pass at the end, so no intermediate state is ever read.
    remap = {}

    def resolve(pid):
        seen = set()
        while pid in remap and pid not in seen:
            seen.add(pid)
            pid = remap[pid]
        return pid

    by_size = sorted(((sum(len(p) for p in c), k) for k, c in comps.items()))
    for size, k in by_size:
        if size >= MIN_PROVINCE:
            break
        info = provs.get(str(k))
        if info is None:
            continue
        own_country = info.get("country_id")
        # Never leave a country with nothing.
        mine = [p for p in provs.values() if p.get("country_id") == own_country]
        if len(mine) <= 1:
            continue
        pixels = [i for c in comps[k] for i in c]
        votes = touching(pid_img, pixels, k)
        # Whoever the neighbour has since become. Adjacency does not change when
        # a province is absorbed, but its identity does.
        merged_votes = defaultdict(int)
        for n, v in votes.items():
            t = resolve(n)
            if t != k:
                merged_votes[t] += v
        if not merged_votes:
            continue
        same = {n: v for n, v in merged_votes.items()
                if provs.get(str(n), {}).get("country_id") == own_country}
        pick = max((same or merged_votes).items(), key=lambda kv: kv[1])[0]
        remap[k] = pick
        merge_data(data, k, pick)
        provs.pop(str(k), None)
        for iso, lst in data.get("claims.json", {}).items():
            data["claims.json"][iso] = [pick if q == k else q for q in lst]
        merged += 1

    if remap:
        # One vectorised pass. Every id that was merged away, including through
        # a chain, resolves to what it finally became.
        final = {src: resolve(src) for src in remap}
        for src, dst in final.items():
            mask = pid_img == src
            pid_img[mask] = dst
            r, g, b = to_rgb(dst)
            img[mask] = (r, g, b)

    # ── 2. Detached pieces big enough to be places ───────────────────────
    comps = components(pid_img)          # re-label: step 1 moved pixels
    for k in sorted(comps):
        pieces = comps[k]
        if len(pieces) < 2:
            continue
        info = provs.get(str(k))
        if info is None:
            continue
        total = sum(len(p) for p in pieces)
        for piece in pieces[1:]:
            if len(piece) < MIN_PIECE:
                continue                  # archipelago pixels; leave them be
            next_id += 1
            new = next_id
            r, g, b = to_rgb(new)
            for i in piece:
                y, x = divmod(i, W)
                pid_img[y, x] = new
                img[y, x] = (r, g, b)
            provs[str(new)] = {
                "color": "#{:06X}".format(new),
                "country_id": info["country_id"],
                "id": new,
                "iso_a3": info.get("iso_a3", ""),
                # Named after where it is rather than after the province it left,
                # which would read as a duplicate on the map.
                "name": "{} (detached)".format(info.get("name", "Province")),
            }
            split_data(data, k, new, len(piece) / float(total))
            split += 1

    if check_only:
        return merged, split, None

    # ── Rewrite ──────────────────────────────────────────────────────────
    out = Image.fromarray(img.astype(np.uint8), "RGB")
    buf = io.BytesIO()
    out.save(buf, format="PNG", optimize=True)
    raw["provinces.png"] = buf.getvalue()
    for f, v in data.items():
        if f in raw:
            raw[f] = (json.dumps(v, separators=(",", ":")) + "\n").encode()

    tmp = path + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as dst:
        for i in members:
            zi = zipfile.ZipInfo(i.filename, date_time=i.date_time)
            zi.compress_type = i.compress_type
            zi.external_attr = i.external_attr
            dst.writestr(zi, raw[i.filename])
    shutil.move(tmp, path)
    return merged, split, len(provs)


def main(argv):
    check = "--check" in argv
    archives = [a for a in argv if not a.startswith("-")]
    if not archives:
        print(__doc__)
        return 2
    rc = 0
    for a in archives:
        merged, split, count = process(a, check)
        name = os.path.basename(a)
        if check:
            if merged or split:
                print("  {}: {} province(s) below the floor, {} detached piece(s) "
                      "to split -- run without --check".format(name, merged, split))
                rc = 1
            else:
                print("  {}: geometry is clean".format(name))
        else:
            print("  {}: merged {} minuscule province(s), split {} detached piece(s), "
                  "{} provinces now".format(name, merged, split, count))
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
