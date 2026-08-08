#!/usr/bin/env python3
"""
Cut new provinces out of the province raster for states too small to have one.

    python3 tools/carve_states.py --check
    python3 tools/carve_states.py

WHY THIS IS DIFFERENT FROM fix_1939_history.py

That tool only ever CHANGES WHO OWNS a province. It cannot add a country that
has no province at all, and at 1279 provinces for the whole world some real
countries do not: Bhutan and Luxembourg are both smaller than a single province
of British India or Belgium. Putting them on the map means editing the
8192x4096 provinces.png itself.

WHAT IT TAKES TO ADD A PROVINCE

A province is not one file. Its id is encoded in its pixel colour, and five
files are keyed by it:

    provinces.png           the pixels, colour = id
    provinces.json          id, owner, and that same colour
    population.json         one integer
    resources.json          deposits
    minorities.json         ethnic makeup
    political_compass.json  local leaning

Miss one and the province exists as far as the map is concerned but has no
population, or no owner, and the game reads a hole. So all six are written
together here.

HOW THE SHAPE IS DECIDED

A bounding box would be wrong -- the boxes for both of these overlap two or
three neighbours, so a box carve would take land from Tibet to build Bhutan and
from Germany to build Luxembourg. Each state is a polygon traced from its real
border, and a pixel is repainted ONLY when it is inside that polygon AND still
owned by the province we intend to take it from. Anything else is left alone,
which is what keeps a carve from eating a neighbour.

The parent province loses the population of what it gave up, in proportion to
the pixels taken. Otherwise the world gains people every time a state is added.

AND WHAT IS LEFT OF THE PARENT HAS TO BE A PROVINCE TOO

If the parent was barely bigger than the state cut out of it, what it keeps is
a shell a pixel or two wide wrapped around the new border -- an id with an
owner, a population line and a compass, that no player can click and that draws
as a white smear when selected. See absorb_rinds, which merges those back into
the country's nearest real province.
"""

import json
import os
import shutil
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STDMAPS = os.path.join(ROOT, "data", "STDmaps")

# Borders traced as (lon, lat). Coarse, but far closer than a rectangle -- and
# at 0.044 degrees per pixel these are 40-80 pixels across, so more detail than
# this would not survive rasterising anyway.
BHUTAN = [(88.75, 27.15), (89.10, 27.32), (89.60, 28.05), (90.00, 28.06),
          (90.70, 28.07), (91.30, 28.05), (91.70, 27.90), (92.05, 27.50),
          (92.10, 27.25), (91.60, 26.85), (91.00, 26.79), (90.30, 26.85),
          (89.60, 26.72), (89.00, 26.85)]

LUXEMBOURG = [(5.74, 49.55), (5.83, 50.09), (6.03, 50.18), (6.24, 49.90),
              (6.53, 49.81), (6.36, 49.47), (6.10, 49.46), (5.79, 49.53)]

# Which maps get which states, and who each one is carved out of.
#
# `parents` are ISO codes: whoever holds that ground in THAT scenario. Bhutan
# comes out of British India, never out of Tibet or China; Luxembourg out of
# whoever holds the Ardennes, which is not the same country in every year.
CARVES = {
    "1914.odmap": [
        dict(iso="BTN", name="Bhutan", poly=BHUTAN, parents=["GBR"],
             color="#e08a3c", population=250000, compass={"left": 5, "auth": 70},
             minorities=[{"n": "Bhutia", "p": 50.0}, {"n": "Nepali", "p": 35.0}],
             treasury=1.5,
             # Bhutan is split on the diagonal, not into bands; the diagonal is
             # the one feature that makes the flag recognisable. The dragon
             # has no equivalent symbol and is left off.
             flag={"type": "diagonal_r", "colors": ["#ffd520", "#ff4e12"]},
             note="A British-protected state with its own monarch since 1907."),
        dict(iso="LUX", name="Luxembourg", poly=LUXEMBOURG, parents=["BEL", "DEU", "GER"],
             color="#4aa8c8", population=260000, compass={"left": 10, "auth": 35},
             minorities=[{"n": "Luxembourgish", "p": 75.0}, {"n": "German", "p": 12.0},
                         {"n": "French", "p": 8.0}],
             treasury=4.0,
             flag={"type": "hstripes_3", "colors": ["#ed2939", "#ffffff", "#00a1de"]},
             note="A sovereign grand duchy, neutral, occupied in 1914 but not annexed."),
    ],
    "1918.odmap": [
        dict(iso="BTN", name="Bhutan", poly=BHUTAN, parents=["GBR"],
             color="#e08a3c", population=255000, compass={"left": 5, "auth": 70},
             minorities=[{"n": "Bhutia", "p": 50.0}, {"n": "Nepali", "p": 35.0}],
             treasury=1.5,
             # Bhutan is split on the diagonal, not into bands; the diagonal is
             # the one feature that makes the flag recognisable. The dragon
             # has no equivalent symbol and is left off.
             flag={"type": "diagonal_r", "colors": ["#ffd520", "#ff4e12"]},
             note="As 1914."),
        dict(iso="LUX", name="Luxembourg", poly=LUXEMBOURG, parents=["BEL", "DEU", "GER"],
             color="#4aa8c8", population=262000, compass={"left": 10, "auth": 35},
             minorities=[{"n": "Luxembourgish", "p": 75.0}, {"n": "German", "p": 12.0},
                         {"n": "French", "p": 8.0}],
             treasury=4.0,
             flag={"type": "hstripes_3", "colors": ["#ed2939", "#ffffff", "#00a1de"]},
             note="Still under German occupation in 1918, still a state."),
    ],
    "1939.odmap": [
        dict(iso="BTN", name="Bhutan", poly=BHUTAN, parents=["GBR"],
             color="#e08a3c", population=280000, compass={"left": 5, "auth": 70},
             minorities=[{"n": "Bhutia", "p": 50.0}, {"n": "Nepali", "p": 35.0}],
             treasury=1.5,
             # Bhutan is split on the diagonal, not into bands; the diagonal is
             # the one feature that makes the flag recognisable. The dragon
             # has no equivalent symbol and is left off.
             flag={"type": "diagonal_r", "colors": ["#ffd520", "#ff4e12"]},
             note="Internally sovereign; Britain guided foreign relations only."),
        dict(iso="LUX", name="Luxembourg", poly=LUXEMBOURG, parents=["BEL", "GER"],
             color="#4aa8c8", population=296000, compass={"left": 10, "auth": 30},
             minorities=[{"n": "Luxembourgish", "p": 75.0}, {"n": "German", "p": 12.0},
                         {"n": "French", "p": 8.0}],
             treasury=5.0,
             flag={"type": "hstripes_3", "colors": ["#ed2939", "#ffffff", "#00a1de"]},
             note="Neutral and independent on 1 September 1939; invaded May 1940."),
    ],
    "1945.odmap": [
        dict(iso="BTN", name="Bhutan", poly=BHUTAN, parents=["GBR", "IND"],
             color="#e08a3c", population=290000, compass={"left": 5, "auth": 68},
             minorities=[{"n": "Bhutia", "p": 50.0}, {"n": "Nepali", "p": 35.0}],
             treasury=1.5,
             # Bhutan is split on the diagonal, not into bands; the diagonal is
             # the one feature that makes the flag recognisable. The dragon
             # has no equivalent symbol and is left off.
             flag={"type": "diagonal_r", "colors": ["#ffd520", "#ff4e12"]},
             note="As 1939."),
        dict(iso="LUX", name="Luxembourg", poly=LUXEMBOURG, parents=["BEL", "GER", "DEU"],
             color="#4aa8c8", population=291000, compass={"left": 10, "auth": 28},
             minorities=[{"n": "Luxembourgish", "p": 75.0}, {"n": "German", "p": 12.0},
                         {"n": "French", "p": 8.0}],
             treasury=4.0,
             flag={"type": "hstripes_3", "colors": ["#ed2939", "#ffffff", "#00a1de"]},
             note="Liberated 1944, a founding member of Benelux."),
    ],
    "1962.odmap": [
        dict(iso="BTN", name="Bhutan", poly=BHUTAN, parents=["IND", "GBR"],
             color="#e08a3c", population=310000, compass={"left": 5, "auth": 65},
             minorities=[{"n": "Bhutia", "p": 50.0}, {"n": "Nepali", "p": 35.0}],
             treasury=2.0,
             # Bhutan is split on the diagonal, not into bands; the diagonal is
             # the one feature that makes the flag recognisable. The dragon
             # has no equivalent symbol and is left off.
             flag={"type": "diagonal_r", "colors": ["#ffd520", "#ff4e12"]},
             note="Sovereign kingdom; India guided foreign relations by the 1949 treaty."),
        dict(iso="LUX", name="Luxembourg", poly=LUXEMBOURG, parents=["BEL", "DEU", "GER"],
             color="#4aa8c8", population=320000, compass={"left": 5, "auth": 25},
             minorities=[{"n": "Luxembourgish", "p": 72.0}, {"n": "German", "p": 12.0},
                         {"n": "French", "p": 9.0}],
             treasury=8.0,
             flag={"type": "hstripes_3", "colors": ["#ed2939", "#ffffff", "#00a1de"]},
             note="EEC founding member."),
    ],
    "map.odmap": [
        dict(iso="BTN", name="Bhutan", poly=BHUTAN, parents=["IND", "CHN"],
             color="#e08a3c", population=790000, compass={"left": 5, "auth": 45},
             minorities=[{"n": "Ngalop", "p": 50.0}, {"n": "Nepali", "p": 35.0}],
             treasury=3.0,
             # Bhutan is split on the diagonal, not into bands; the diagonal is
             # the one feature that makes the flag recognisable. The dragon
             # has no equivalent symbol and is left off.
             flag={"type": "diagonal_r", "colors": ["#ffd520", "#ff4e12"]},
             note="Present day."),
        dict(iso="LUX", name="Luxembourg", poly=LUXEMBOURG, parents=["BEL", "DEU", "FRA"],
             color="#4aa8c8", population=660000, compass={"left": 10, "auth": 20},
             minorities=[{"n": "Luxembourgish", "p": 52.0}, {"n": "Portuguese", "p": 16.0},
                         {"n": "French", "p": 8.0}],
             treasury=30.0,
             flag={"type": "hstripes_3", "colors": ["#ed2939", "#ffffff", "#00a1de"]},
             note="Present day."),
    ],
}


# What is left of a gutted province is debris, not a province. See
# absorb_rinds. The test is what the CARVE did to it, not what shape it is:
# a province that gave up three quarters of itself and kept a few hundred
# pixels was destroyed by the carve, whatever the remainder happens to look
# like. Shape is the symptom; this is the cause.
RIND_KEEP_FRACTION = 0.25
RIND_MAX_PX = 800


def absorb_rinds(im, donors, prov, pop, res, mino, comp, extra, check):
    """Merge away the hairline shells a carve can leave behind.

    CUTTING A STATE OUT OF A PROVINCE ITS OWN SIZE LEAVES A RIND.

    carve_states repaints the pixels inside a traced outline and lets the
    parent keep the rest. That is right when the parent is Assam and the state
    is Bhutan-shaped -- but on the 1914 and 1918 maps the parent province was
    itself barely bigger than Bhutan, so what it kept was a 262 px shell
    wrapped around the new state, one to two pixels wide along its north and
    west.

    That shell is a province by every mechanical test: it has an id, an owner,
    a population line and a compass. It is not one by any useful one. You
    cannot reliably click it, and selecting it draws a white bar with no
    interior -- which is how it was found, someone asking what they were
    looking at.

    The ground stays with the country that held it; it just stops being its own
    province, merging into that country's largest neighbouring province. Giving
    it to the carved state instead would be easier and wrong: Bhutan would end
    up bigger than Bhutan.
    """
    import numpy as np
    from PIL import Image

    arr = np.array(im.convert("RGB"), dtype=np.uint32)
    ids = arr[:, :, 0] << 16 | arr[:, :, 1] << 8 | arr[:, :, 2]
    merged = []

    for donor, lost in sorted(donors.items()):
        m = ids == donor
        n = int(m.sum())
        if n == 0 or n > RIND_MAX_PX or n >= RIND_KEEP_FRACTION * (n + lost):
            continue
        owner = prov.get(str(donor), {}).get("country_id")
        if owner is None:
            continue

        grow = np.zeros_like(m)
        grow[1:, :] |= m[:-1, :]
        grow[:-1, :] |= m[1:, :]
        grow[:, 1:] |= m[:, :-1]
        grow[:, :-1] |= m[:, 1:]
        best, best_n = None, 0
        for cand in np.unique(ids[grow & ~m]).tolist():
            if cand == 0 or cand == donor:
                continue
            if prov.get(str(cand), {}).get("country_id") != owner:
                continue
            area = int((ids == cand).sum())
            if area > best_n:
                best, best_n = cand, area
        if best is None:
            print(f"    rind prov {donor} ({n} px) has no neighbour of its own "
                  f"country -- left alone")
            continue

        merged.append((donor, best, n, lost))
        if check:
            continue

        ids[m] = best
        pop[str(best)] = pop.get(str(best), 0) + pop.pop(str(donor), 0)
        dres = res.pop(str(donor), None)
        if isinstance(dres, dict):
            keep = res.setdefault(str(best), {})
            if isinstance(keep, dict):
                for k, v in dres.items():
                    if isinstance(v, dict) and isinstance(keep.get(k), dict):
                        for part in ("a", "b"):
                            keep[k][part] = round(float(keep[k].get(part, 0))
                                                  + float(v.get(part, 0)), 3)
                    elif isinstance(v, (int, float)) and isinstance(keep.get(k), (int, float)):
                        keep[k] = round(float(keep[k]) + float(v), 3)
                    else:
                        keep.setdefault(k, v)
        mino.pop(str(donor), None)
        comp.pop(str(donor), None)
        prov.pop(str(donor), None)
        # Anything else keyed by province id moves with the ground rather than
        # being deleted with the id -- an army standing there is still standing
        # there, and a port is still a port.
        for obj in extra.values():
            if not isinstance(obj, dict) or str(donor) not in obj:
                continue
            moved = obj.pop(str(donor))
            if str(best) in obj and isinstance(obj[str(best)], list) and isinstance(moved, list):
                obj[str(best)].extend(moved)
            else:
                obj.setdefault(str(best), moved)

    for donor, best, n, lost in merged:
        print(f"    rind prov {donor}: kept {n} px of {n + lost}, merged into prov {best}")
    if merged and not check:
        arr[:, :, 0] = (ids >> 16) & 0xFF
        arr[:, :, 1] = (ids >> 8) & 0xFF
        arr[:, :, 2] = ids & 0xFF
        return Image.fromarray(arr.astype("uint8"), "RGB"), len(merged)
    return im, len(merged)


def inside(poly, lon, lat):
    """Ray casting. Small polygons, called per pixel of a small box only."""
    hit = False
    n = len(poly)
    for i in range(n):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % n]
        if (y0 > lat) != (y1 > lat):
            xx = x0 + (lat - y0) * (x1 - x0) / (y1 - y0)
            if lon < xx:
                hit = not hit
    return hit


def carve_map(path, states, check):
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = None

    work = tempfile.mkdtemp(prefix="carve_")
    try:
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            z.extractall(work)

        need = ["provinces.png", "provinces.json", "countries.json",
                "population.json", "resources.json", "minorities.json",
                "political_compass.json"]
        for fn in need:
            if fn not in names:
                print(f"  {os.path.basename(path)}: no {fn}, skipped")
                return 0

        prov = json.load(open(os.path.join(work, "provinces.json")))
        ctry = json.load(open(os.path.join(work, "countries.json")))
        pop = json.load(open(os.path.join(work, "population.json")))
        res = json.load(open(os.path.join(work, "resources.json")))
        mino = json.load(open(os.path.join(work, "minorities.json")))
        comp = json.load(open(os.path.join(work, "political_compass.json")))
        cc_path = os.path.join(work, "country_compass.json")
        cc = json.load(open(cc_path)) if os.path.exists(cc_path) else {}

        by_iso = {v["iso_a3"]: int(k) for k, v in ctry.items()}
        im = Image.open(os.path.join(work, "provinces.png")).convert("RGB")
        W, H = im.size
        px = im.load()

        # Anything else keyed by province id, so a rind merge can carry it
        # across instead of orphaning it. Loaded only if the map has it.
        extra = {}
        for fn in ("armies.json", "ports.json", "claims.json"):
            fp = os.path.join(work, fn)
            if os.path.exists(fp):
                extra[fn] = json.load(open(fp))

        next_pid = max(int(k) for k in prov) + 1
        next_cid = max(int(k) for k in ctry if int(k) < 60000) + 1
        changed = 0
        donors = {}          # province id -> pixels the carves took from it

        for st in states:
            if st["iso"] in by_iso:
                print(f"    skip {st['iso']}: already on this map")
                continue
            parent_cids = {by_iso[p] for p in st["parents"] if p in by_iso}
            if not parent_cids:
                print(f"    skip {st['iso']}: none of {st['parents']} on this map")
                continue
            parent_pids = {int(k) for k, v in prov.items()
                           if v["country_id"] in parent_cids}

            lons = [p[0] for p in st["poly"]]
            lats = [p[1] for p in st["poly"]]
            x0 = max(0, int((min(lons) + 180) / 360 * W) - 2)
            x1 = min(W - 1, int((max(lons) + 180) / 360 * W) + 2)
            y0 = max(0, int((90 - max(lats)) / 180 * H) - 2)
            y1 = min(H - 1, int((90 - min(lats)) / 180 * H) + 2)

            pid = next_pid
            taken = {}
            for y in range(y0, y1 + 1):
                lat = 90 - (y + 0.5) / H * 180
                for x in range(x0, x1 + 1):
                    lon = (x + 0.5) / W * 360 - 180
                    if not inside(st["poly"], lon, lat):
                        continue
                    r, g, b = px[x, y]
                    cur = (r << 16) | (g << 8) | b
                    if cur not in parent_pids:      # sea, or somebody else's
                        continue
                    px[x, y] = ((pid >> 16) & 255, (pid >> 8) & 255, pid & 255)
                    taken[cur] = taken.get(cur, 0) + 1

            total = sum(taken.values())
            if total < 8:
                print(f"    skip {st['iso']}: only {total} px would be carved")
                continue

            # POPULATION COMES FROM HISTORY, NOT FROM AREA.
            #
            # The first version scaled the parent's population by the fraction
            # of its pixels taken, and gave Bhutan 1.38 million people -- the
            # population Assam would have at Assam's density. Bhutan is
            # mountains; it had about 280,000. Area is a bad proxy for people
            # exactly where these small states are.
            #
            # So the recorded figure is used, and the parent loses precisely
            # that many rather than a share of itself. The world total is
            # unchanged either way, but now both numbers are right.
            head = st["population"]
            biggest = max(taken.items(), key=lambda kv: kv[1])[0]
            have = pop.get(str(biggest), 0)
            pop[str(biggest)] = max(0, have - head)

            cid = next_cid
            next_cid += 1
            ctry[str(cid)] = {
                "id": cid, "iso_a3": st["iso"], "name": st["name"],
                "color": st["color"], "flag_actual": st["flag"],
                "flag_censored": {"type": "solid", "colors": [st["color"]],
                                  "censored": True},
                "treasury": st["treasury"],
            }
            cc[st["iso"]] = st["compass"]

            prov[str(pid)] = {"id": pid, "name": "", "country_id": cid,
                              "iso_a3": st["iso"], "color": "#%06x" % pid}
            pop[str(pid)] = head
            # A donated share of the parent's deposits, not a new endowment.
            # Shapes vary between maps -- some entries are {res: {a,b}}, others
            # are plain numbers -- so anything unexpected yields no deposits
            # rather than a malformed entry the loader would trip on.
            donor = next(iter(taken))
            dres = res.get(str(donor))
            share = {}
            if isinstance(dres, dict):
                for k, v in dres.items():
                    if isinstance(v, dict):
                        share[k] = {"a": round(float(v.get("a", 0)) * 0.02, 3),
                                    "b": round(float(v.get("b", 0)) * 0.02, 3)}
                    elif isinstance(v, (int, float)):
                        share[k] = round(float(v) * 0.02, 3)
            res[str(pid)] = share
            mino[str(pid)] = st["minorities"]
            comp[str(pid)] = st["compass"]

            next_pid += 1
            changed += 1
            for src, cnt in taken.items():
                donors[src] = donors.get(src, 0) + cnt
            src_desc = ", ".join(f"{n}px from prov {s}" for s, n in sorted(taken.items()))
            print(f"    {st['iso']}  {st['name']:12s} pid {pid:5d} cid {cid:3d}  "
                  f"{total:5d} px  ({src_desc})")

        if donors:
            im, absorbed = absorb_rinds(im, donors, prov, pop, res, mino, comp,
                                        extra, check)
            changed += absorbed

        if changed == 0 or check:
            if check:
                print("    --check: nothing written")
            return changed

        im.save(os.path.join(work, "provinces.png"))
        for fn, obj in (("provinces.json", prov), ("countries.json", ctry),
                        ("population.json", pop), ("resources.json", res),
                        ("minorities.json", mino), ("political_compass.json", comp)):
            with open(os.path.join(work, fn), "w", encoding="utf-8") as f:
                json.dump(obj, f, separators=(",", ":"))
        if os.path.exists(cc_path):
            with open(cc_path, "w", encoding="utf-8") as f:
                json.dump(cc, f, separators=(",", ":"))
        for fn, obj in extra.items():
            with open(os.path.join(work, fn), "w", encoding="utf-8") as f:
                json.dump(obj, f, separators=(",", ":"))

        tmp = path + ".tmp"
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
            for name in names:
                z.write(os.path.join(work, name), name)
        os.replace(tmp, path)
        return changed
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    check = "--check" in sys.argv
    only = [a for a in sys.argv[1:] if not a.startswith("-")]
    total = 0
    for fname, states in CARVES.items():
        if only and fname not in only:
            continue
        path = os.path.join(STDMAPS, fname)
        if not os.path.exists(path):
            print(f"  {fname}: not found, skipped")
            continue
        print(f"\n{fname}")
        total += carve_map(path, states, check)
    print(f"\n{total} province(s) carved")
    return 0


if __name__ == "__main__":
    sys.exit(main())
