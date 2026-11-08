#!/usr/bin/env python3
"""Build an outbreak scenario from an existing .odmap.

    python3 tools/outbreak/make_outbreak.py data/STDmaps/map.odmap \
            -o data/STDmaps/outbreak.odmap --seed-country CHN

What it adds to the archive is small and entirely data:

  * one country, PLG, with a free id -- the outbreak itself, which the game
    treats as any other owner of provinces;
  * scripts/outbreak.txt, from outbreak.tmpl.txt, with patient zero's province
    ids filled in;
  * a 2x1 flag, because a country without one has nothing to draw;
  * a metadata name and description.

Nothing else changes: the raster, the populations, the armies and every other
layer are copied through byte for byte. The spread is the script's, and the
script only needs `foreach neighbour`, `foreach province in array.NAME` and
`random.N` -- engine version 5.
"""
import argparse
import json
import os
import struct
import sys
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(HERE, "outbreak.tmpl.txt")

# Anything at or above this is a rebel or a sentinel (Game::REBEL_CID_MIN,
# UNC_CID, BLC_CID, SPC_CID), so an outbreak must sit below it.
REBEL_CID_MIN = 60000


def solid_png(rgb, w=2, h=1):
    """A solid PNG, written by hand so this script needs no Pillow."""
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="the .odmap to build from")
    ap.add_argument("-o", "--out", required=True, help="the .odmap to write")
    ap.add_argument("--seed-country", default=None,
                    help="ISO A3 of the country patient zero starts in "
                         "(default: the one with the most provinces)")
    ap.add_argument("--seeds", type=int, default=1, help="provinces to start in (default 1)")
    ap.add_argument("--chance", type=int, default=22,
                    help="%% chance of crossing into an open province per turn (default 22)")
    ap.add_argument("--fort-divisor", type=int, default=4,
                    help="that chance is divided by this where forts stand (default 4)")
    ap.add_argument("--name", default="Outbreak", help="the map's name in the browser")
    ap.add_argument("--colour", default="#6b7f3a", help="the outbreak's map colour")
    args = ap.parse_args()

    if args.seeds < 1:
        sys.exit("--seeds must be at least 1")
    if not 1 <= args.chance <= 100:
        sys.exit("--chance must be a percentage between 1 and 100")
    if args.fort_divisor < 1:
        sys.exit("--fort-divisor must be at least 1")

    with zipfile.ZipFile(args.source) as z:
        names = z.namelist()
        for need in ("countries.json", "provinces.json"):
            if need not in names:
                sys.exit(f"{args.source} has no {need} -- is it an .odmap?")
        countries = json.loads(z.read("countries.json"))
        provinces = json.loads(z.read("provinces.json"))
        metadata = json.loads(z.read("metadata.json")) if "metadata.json" in names else {}
        payload = {n: z.read(n) for n in names}

    # ── who patient zero starts on ──
    # provinces.json keys are province ids; the owner field has been spelled
    # a couple of ways across generator versions, so try each.
    def owner_of(p):
        for key in ("country_id", "countryId", "owner", "cid"):
            if isinstance(p, dict) and key in p:
                return p[key]
        return None

    by_owner = {}
    for pid, prov in provinces.items():
        o = owner_of(prov)
        if o is None:
            continue
        by_owner.setdefault(int(o), []).append(int(pid))
    if not by_owner:
        sys.exit("could not read any province owners out of provinces.json")

    iso_of = {}
    for cid, c in countries.items():
        iso = c.get("iso_a3") or c.get("isoA3")
        if iso:
            iso_of[iso] = int(cid)

    if args.seed_country:
        if args.seed_country not in iso_of:
            sys.exit(f"{args.seed_country} is not on this map; it has "
                     f"{len(iso_of)} countries")
        seed_cid = iso_of[args.seed_country]
        seed_iso = args.seed_country
    else:
        # The biggest real country, so patient zero has somewhere to spread to.
        seed_cid = max((c for c in by_owner if 0 < c < REBEL_CID_MIN),
                       key=lambda c: len(by_owner[c]))
        seed_iso = next((i for i, c in iso_of.items() if c == seed_cid), str(seed_cid))

    owned = sorted(by_owner.get(seed_cid, []))
    if not owned:
        sys.exit(f"{seed_iso} owns no provinces on this map")
    if args.seeds > len(owned):
        sys.exit(f"{seed_iso} has {len(owned)} provinces; --seeds {args.seeds} "
                 "asks for more than that")
    # From the middle of its id range rather than the first: province ids run
    # roughly in scan order, so the lowest id of a big country tends to be on
    # its edge, and an outbreak that starts on a coast spends its first turns
    # spreading into the sea it cannot cross.
    mid = len(owned) // 2
    seeds = owned[mid:mid + args.seeds]

    # ── the outbreak's own country id ──
    used = {int(c) for c in countries}
    cid = next(i for i in range(1, REBEL_CID_MIN) if i not in used)
    if "PLG" in iso_of:
        sys.exit(f"{args.source} already has a PLG country -- is it already an outbreak map?")

    countries[str(cid)] = {
        "id": cid,
        "iso_a3": "PLG",
        "name": "Outbreak",
        "color": args.colour,
        "treasury": 0.0,
        "flag_actual": {"image": "flags/PLG.png"},
        "flag_censored": {"image": "flags/PLG.png"},
    }

    # ── the script ──
    tmpl = open(TEMPLATE, encoding="utf-8").read()
    seed_lines = "\n".join(f'  set province.{p}.owner to "PLG"' for p in seeds)
    script = (tmpl.replace("__SEEDS__", seed_lines)
                  .replace("__SEED_NAME__", seed_iso)
                  .replace("__CHANCE__", str(args.chance))
                  .replace("__FORTS__", str(args.fort_divisor)))
    for hole in ("__SEEDS__", "__SEED_NAME__", "__CHANCE__", "__FORTS__"):
        assert hole not in script, f"{hole} was left unfilled"

    metadata["name"] = args.name
    metadata["description"] = (
        f"Something is spreading out of {seed_iso}, one province a turn, and it "
        "does not take letters. Forts slow it. Nothing else does.")
    metadata["has_scripts"] = True

    payload["countries.json"] = json.dumps(countries, indent=2).encode()
    payload["metadata.json"] = json.dumps(metadata, indent=2).encode()
    payload["scripts/outbreak.txt"] = script.encode()
    payload["flags/PLG.png"] = solid_png(bytes.fromhex(args.colour.lstrip("#")))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as out:
        for name in payload:
            out.writestr(name, payload[name])

    print(f"{args.out}")
    print(f"  outbreak    PLG, country id {cid}, colour {args.colour}")
    print(f"  patient zero {', '.join(str(p) for p in seeds)}  (in {seed_iso}, "
          f"which holds {len(owned)})")
    print(f"  spread       {args.chance}% a turn, {args.chance // args.fort_divisor}% "
          f"into a fortified province")
    print(f"  script       scripts/outbreak.txt, {len(script.splitlines())} lines, "
          "engine version 5")


if __name__ == "__main__":
    main()
