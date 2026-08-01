#!/usr/bin/env python3
"""
Check every province against a real historical boundary dataset.

    export OD_HISTGEO_DIR=/path/to/historical-basemaps/geojson
    python3 tools/check_map_history.py               # all scenarios
    python3 tools/check_map_history.py --map 1939    # one
    python3 tools/check_map_history.py --min-px 200  # ignore slivers

THIS IS A DEVELOPMENT TOOL AND SHIPS NOTHING

It reads a third-party boundary dataset from a directory you point it at. That
directory is deliberately NOT in this repository and nothing it contains is
copied into the game. The tool prints a report; a human reads the report and
edits our own tables (fix_map_history.py, carve_borders.py). What we ship is
province-to-country assignments at our own raster's resolution -- which country
held which ground in a given year is a historical fact, not the dataset's
expression of it.

Get the data with:

    git clone https://github.com/aourednik/historical-basemaps /tmp/histbase
    export OD_HISTGEO_DIR=/tmp/histbase/geojson

WHY THIS EXISTS

Every scenario was built from a modern province raster, so each inherited the
borders of the wrong century, and the errors were found only when somebody
noticed one by eye -- which is how The Gathering Storm shipped for so long with
Poland reaching the Oder-Neisse line, a border drawn six years after that
scenario opens. Checking by eye does not scale to six maps and 8,000 provinces.

HOW IT WORKS

The historical polygons are rasterised onto the same lat/lon grid as
provinces.png. For each province, the tool takes the polity covering the most
of its pixels and compares that against the country the scenario gives it.
Colonies resolve through SUBJECTO, so British India reports as the United
Kingdom rather than as itself.

READ THE REPORT, DO NOT AUTOMATE IT

The dataset's snapshot years do not all line up with our scenario dates, and
the mismatch is the point of several scenarios. 1918 is checked against 1920,
by which time Austria-Hungary has already been partitioned; 1939 is checked
against 1938, before the Polish campaign. Every disagreement is a question,
not a defect.
"""

import argparse
import io
import json
import os
import sys
import zipfile

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAPS_DIR = os.path.join(ROOT, "data", "STDmaps")

# scenario -> the dataset's nearest snapshot, and how far off it is.
YEARS = {
    "1914.odmap": ("world_1914.geojson", "exact"),
    # No dataset has 1918, so this one is BRACKETED: see BRACKET below.
    "1918.odmap": ("world_1920.geojson",
                   "bracketed between world_1914 and world_1920"),
    "1939.odmap": ("world_1938.geojson",
                   "one year early: before Prague, Memel and the Polish campaign"),
    "1945.odmap": ("world_1945.geojson", "exact"),
    "1962.odmap": ("world_1960.geojson",
                   "two years early: before the 1962 African independences"),
    "map.odmap":  ("world_2000.geojson", "exact"),
}

# A scenario with no snapshot of its own is checked against the snapshot BEFORE
# it and the snapshot AFTER it. Where the two agree, that is what held the
# ground in between and a disagreement with us is a real finding. Where they
# differ, the border moved during the gap and only history can say which side
# of the change our date falls on -- those are reported separately rather than
# counted as errors.
#
# October 1918 needs this badly. Checked against 1920 alone it showed 96
# disagreements, nearly all of them the post-war settlement: Poland exists,
# Austria-Hungary is partitioned, the Baltic states are independent. None of
# that had happened when the scenario opens.
BRACKET = {
    "1918.odmap": ("world_1914.geojson", "world_1920.geojson",
                   "July 1914", "1920"),
}

# Our ISO -> the names the dataset uses for the same sovereign. Only needed
# where the two disagree; anything unlisted is matched on name, case-folded.
ALIAS = {
    # The scenarios model the Dominions as one British Empire. That is a design
    # choice, not an error, so it must not fill the report with 200 rows.
    # The dataset's SUBJECTO usually names the COLONY rather than the power
    # that held it -- "Angola", not "Portugal". So a colony sitting under its
    # real coloniser has to be listed here or every empire reads as an error.
    "GBR": {"united kingdom", "united kingdom of great britain and ireland",
            "great britain", "british empire",
            "canada", "australia", "new zealand", "south africa",
            "union of south africa", "newfoundland", "india", "british india",
            "ireland", "nigeria", "sudan", "malaya", "guyana", "british guiana",
            "papua new guinea", "kenya", "zambia", "botswana", "bechuanaland",
            "pakistan", "burma", "namibia", "ceylon", "gold coast", "somaliland",
            "tanzania, united republic of", "tanganyika", "palestine",
            "transjordan", "cyprus", "malta", "sierra leone", "gambia",
            "uganda", "rhodesia", "nyasaland", "aden", "egypt"},
    "GER": {"germany", "german empire", "german reich", "third reich"},
    "GEA": {"germany", "allied occupied germany"},
    "SOV": {"soviet union", "ussr", "russia", "russian empire", "russian sfsr"},
    "RUS": {"russia", "russian empire", "soviet union", "ussr", "russian sfsr",
            "far eastern ssr", "finland"},
    "USA": {"united states", "united states of america"},
    "FRA": {"france", "french republic", "algeria", "madagascar", "kamerun",
            "central african republic", "french guiana", "french indochina",
            "laos", "cambodia", "vietnam", "tunisia", "morocco", "syria",
            "lebanon", "senegal", "mali", "niger", "chad", "gabon", "congo"},
    "CHN": {"china", "republic of china", "qing", "manchu empire"},
    "TUR": {"turkey", "ottoman empire"},
    "IRN": {"iran", "persia"},
    "THA": {"thailand", "siam", "rattanakosin kingdom"},
    "LKA": {"sri lanka", "ceylon"},
    "MMR": {"myanmar", "burma"},
    "DEU": {"germany", "west germany", "federal republic of germany"},
    "DDR": {"east germany", "german democratic republic"},
    "PRK": {"north korea", "korea", "korea, democratic peoples republic of"},
    "KOR": {"south korea", "korea", "korea, republic of"},
    "AUS": {"australia", "papua new guinea"},
    "COG": {"congo", "republic of the congo brazzaville"},
    "MYS": {"malaysia", "malaya", "federation of malaya"},
    "BFA": {"burkina faso", "upper volta"},
    "ZAF": {"south africa", "namibia", "south west africa"},
    "MAR": {"morocco", "western sahara"},
    "XSO": {"somaliland", "somalia"},
    "OTT": {"ottoman empire", "turkey", "arabia nejd", "nejd", "hejaz",
            "emirate of bin shalan", "yemen"},
    "ETH": {"ethiopia", "abyssinia", "ethiopian empire"},
    "JPN": {"japan", "empire of japan", "occupied japan"},
    "NLD": {"netherlands", "holland", "netherlands indies",
            "dutch east indies", "suriname"},
    "AUH": {"austria-hungary", "austria hungary", "austro-hungarian empire"},
    "YUG": {"yugoslavia", "kingdom of yugoslavia", "serbia"},
    "CSK": {"czechoslovakia"},
    "IDN": {"indonesia", "dutch east indies", "netherlands"},
    "VNM": {"vietnam", "north vietnam", "democratic republic of vietnam"},
    "RVN": {"south vietnam", "republic of vietnam"},
    "COD": {"dem. rep. congo", "democratic republic of the congo", "zaire",
            "belgian congo", "congo"},
    "MCK": {"manchukuo", "manchuria"},
    "TIB": {"tibet"},
    "MNG": {"mongolia", "mongolian people's republic"},
    "ITA": {"italy", "italian republic", "kingdom of italy", "libya",
            "italian somaliland", "eritrea", "abyssinia"},
    "ESP": {"spain", "spanish state"},
    "POL": {"poland", "second polish republic", "polish republic"},
    "TZA": {"tanzania", "tanzania, united republic of", "tanganyika"},
    "CAF": {"central african republic"},
    "CIV": {"ivory coast", "cote divoire", "côte d'ivoire"},
    "BLR": {"belarus", "byelarus", "byelorussia"},
    "BEL": {"belgium", "zaire", "belgian congo"},
    "PRT": {"portugal", "portuguese state", "angola", "mozambique"},
    "DNK": {"denmark", "greenland", "iceland", "faeroe is."},
    "SRB": {"serbia", "yugoslavia", "fr yugoslavia"},
    "CZE": {"czechia", "czech republic", "czechoslovakia"},
    "BIH": {"bosnia and herz.", "bosnia and herzegovina"},
    "MKD": {"north macedonia", "macedonia"},
    "DOM": {"dominican rep.", "dominican republic"},
    "SAU": {"saudi arabia", "arabia", "nejd", "hejaz",
            "emirate of bin shal'an"},
}


def norm(s):
    """Strip the forms of address so 'Kingdom of Italy' matches 'Italy'.

    Without this the report is mostly false positives: the scenarios name
    countries the way a period atlas would and the dataset names them plainly.
    """
    s = (s or "").lower().strip()
    for ch in ".,()'":
        s = s.replace(ch, "")
    drop = ("kingdom of the", "kingdom of", "republic of the", "republic of",
            "empire of the", "empire of", "dominion of the", "dominion of",
            "state of the", "state of", "union of the", "union of",
            "federation of the", "federation of", "principality of",
            "sublime state of", "peoples republic of", "democratic republic of",
            "the ")
    changed = True
    while changed:
        changed = False
        for d in drop:
            if s.startswith(d + " ") or s.startswith(d):
                s = s[len(d):].strip()
                changed = True
    for suf in (" empire", " republic", " kingdom", " reich", " sfr", " sfsr"):
        if s.endswith(suf):
            s = s[: -len(suf)].strip()
    for pre in ("second ", "first ", "third ", "greater ", "occupied ",
                "allied occupied "):
        if s.startswith(pre):
            s = s[len(pre):].strip()
    return s


def sovereign(props):
    """Who actually held this ground: a colony answers to its metropole."""
    s = (props.get("SUBJECTO") or "").strip()
    n = (props.get("NAME") or "").strip()
    return (s or n), n


def rasterise(path, W, H):
    """Paint the historical polygons onto our lat/lon grid."""
    with open(path, encoding="utf-8") as f:
        gj = json.load(f)
    idx = Image.new("I", (W, H), 0)
    d = ImageDraw.Draw(idx)
    table = {}
    for i, feat in enumerate(gj["features"], start=1):
        table[i] = sovereign(feat.get("properties") or {})
        geom = feat.get("geometry") or {}
        polys = []
        if geom.get("type") == "Polygon":
            polys = [geom["coordinates"]]
        elif geom.get("type") == "MultiPolygon":
            polys = geom["coordinates"]
        for poly in polys:
            if not poly:
                continue
            ring = poly[0]
            pts = [((lon + 180.0) / 360.0 * W, (90.0 - lat) / 180.0 * H)
                   for lon, lat in ring]
            if len(pts) >= 3:
                d.polygon(pts, fill=i)
    return np.array(idx), table


def check(name, gj_dir, min_px, limit, confident):
    gj_name, caveat = YEARS[name]
    gj_path = os.path.join(gj_dir, gj_name)
    if not os.path.exists(gj_path):
        print(f"  missing {gj_path}", file=sys.stderr)
        return 1

    with zipfile.ZipFile(os.path.join(MAPS_DIR, name)) as z:
        countries = json.loads(z.read("countries.json"))
        provinces = json.loads(z.read("provinces.json"))
        meta = json.loads(z.read("metadata.json"))
        arr = np.array(Image.open(io.BytesIO(z.read("provinces.png"))).convert("RGB"))

    arr = arr[::4, ::4]
    H, W = arr.shape[:2]
    pid = ((arr[:, :, 0].astype(np.int64) << 16)
           | (arr[:, :, 1].astype(np.int64) << 8)
           | arr[:, :, 2].astype(np.int64))
    hist, table = rasterise(gj_path, W, H)

    # Bracketed scenarios get a second, earlier snapshot to compare against.
    hist2 = table2 = None
    if name in BRACKET:
        before, after, lo_label, hi_label = BRACKET[name]
        p2 = os.path.join(gj_dir, before)
        if os.path.exists(p2):
            hist2, table2 = rasterise(p2, W, H)

    iso_of = {int(k): v["iso_a3"] for k, v in countries.items()}
    name_of = {int(k): v["name"] for k, v in countries.items()}

    print(f"\n{'='*78}\n{name}  {meta.get('name')}  [{meta.get('map_date')}]"
          f"\nchecked against {gj_name} -- {caveat}\n{'='*78}")

    rows, changed, straddle = [], [], []
    for p in np.unique(pid):
        if p == 0:
            continue
        pv = provinces.get(str(int(p)))
        if pv is None:
            continue
        sel = pid == p
        n = int(sel.sum())
        if n < min_px:
            continue
        vals, counts = np.unique(hist[sel], return_counts=True)
        keep = vals != 0
        if not keep.any():
            continue
        vals, counts = vals[keep], counts[keep]
        top = int(vals[np.argmax(counts)])
        share = int(counts.max()) / n
        sov, polity = table[top]
        ours = iso_of.get(pv["country_id"], "?")
        # Ocean, ice and genuinely unclaimed ground: the dataset has no polity
        # there and neither do we. Agreement, not a finding.
        if not sov.strip() or ours in ("UNC", "BLC", "SPC"):
            continue
        want = {norm(x) for x in ALIAS.get(ours, {ours})}
        want |= {norm(name_of.get(pv["country_id"], "")), ours.lower()}
        if norm(sov) in want or norm(polity) in want:
            continue
        if hist2 is not None:
            v2, c2 = np.unique(hist2[sel], return_counts=True)
            k2 = v2 != 0
            sov2 = polity2 = ""
            if k2.any():
                v2, c2 = v2[k2], c2[k2]
                sov2, polity2 = table2[int(v2[np.argmax(c2)])]
            # The earlier snapshot agrees with us: the border moved AFTER our
            # date, so the later snapshot is describing a change we predate.
            if norm(sov2) in want or norm(polity2) in want:
                changed.append((n, int(p), ours, name_of.get(pv["country_id"], ""),
                                sov, sov2, share))
                continue
            # Both snapshots say someone else. That is a real finding.
            if norm(sov2) != norm(sov):
                straddle.append((n, int(p), ours, name_of.get(pv["country_id"], ""),
                                 sov, sov2, share))
                continue
        # A province straddling a border is a carving question, not a wrong
        # owner. Rank by how much of it the other side actually holds.
        if share < confident:
            continue
        rows.append((n, int(p), ours, name_of.get(pv["country_id"], ""),
                     sov, polity, share))

    rows.sort(reverse=True)
    print(f"{len(rows)} provinces disagree with history "
          f"(of {len(np.unique(pid))-1} checked; ignoring under {min_px}px "
          f"and splits under {confident*100:.0f}%)\n")
    for n, p, ours, oursname, sov, polity, share in rows[:limit]:
        extra = f" (as {polity})" if polity and polity != sov else ""
        print(f"  prov {p:5d} {n:6d}px  you: {ours:4s} {oursname[:24]:24s} "
              f"| history: {sov}{extra}  [{share*100:.0f}% of the province]")
    if len(rows) > limit:
        print(f"  ... and {len(rows)-limit} more (raise --limit to see them)")

    if hist2 is not None:
        before, after, lo_label, hi_label = BRACKET[name]
        print(f"\n  {len(changed)} more agree with {lo_label} but not {hi_label} "
              f"-- the border moved after this scenario opens, so we are right:")
        for n, p, ours, oursname, sov, sov2, share in sorted(changed, reverse=True)[:8]:
            print(f"     prov {p:5d} {n:6d}px  {ours:4s} | {hi_label} says {sov}")
        if len(changed) > 8:
            print(f"     ... and {len(changed)-8} more")
        if straddle:
            print(f"\n  {len(straddle)} disagree with BOTH snapshots and with "
                  f"each other -- judgement calls:")
            for n, p, ours, oursname, sov, sov2, share in sorted(straddle, reverse=True)[:10]:
                print(f"     prov {p:5d} {n:6d}px  you: {ours:4s} | "
                      f"{lo_label}: {sov2} | {hi_label}: {sov}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map")
    ap.add_argument("--min-px", type=int, default=60,
                    help="ignore provinces smaller than this many sampled pixels")
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--confident", type=float, default=0.75,
                    help="only report when the other side holds at least "
                         "this share of the province")
    ap.add_argument("--geojson-dir", default=os.environ.get("OD_HISTGEO_DIR"))
    a = ap.parse_args()

    if not a.geojson_dir:
        print(__doc__.strip(), file=sys.stderr)
        print("\nSet OD_HISTGEO_DIR or pass --geojson-dir.", file=sys.stderr)
        return 2

    only = a.map
    if only and not only.endswith(".odmap"):
        only += ".odmap"
    rc = 0
    for nm in YEARS:
        if only and nm != only:
            continue
        rc |= check(nm, a.geojson_dir, a.min_px, a.limit, a.confident)
    return rc


if __name__ == "__main__":
    sys.exit(main())
