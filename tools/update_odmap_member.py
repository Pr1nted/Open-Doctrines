#!/usr/bin/env python3
"""Replace one file inside .odmap archives, leaving every other member alone.

A .odmap carries its own copy of the data files a map needs -- policies.json
among them -- and the archive's copy WINS over data/ (see Game::loadPolicies).
So editing data/policies.json changes nothing for a shipped map until the
archives are updated too.

Regenerating the maps to change one JSON member would re-derive population,
resources and everything else from the network, which is both slow and a way to
lose corrected data. This rewrites the archive with one member swapped and the
rest copied byte for byte.

  python3 tools/update_odmap_member.py policies.json data/policies.json \
      data/STDmaps/*.odmap
"""
import os
import shutil
import sys
import zipfile


def update(archive, member, payload):
    tmp = archive + ".tmp"
    replaced = False
    with zipfile.ZipFile(archive) as src:
        infos = src.infolist()
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as dst:
            for info in infos:
                data = payload if info.filename == member else src.read(info.filename)
                if info.filename == member:
                    replaced = True
                # Keep each member's own compression: land_sea.png and the flags
                # are already-compressed payloads that gain nothing from being
                # deflated again, and the archive's size budget assumes that.
                zi = zipfile.ZipInfo(info.filename, date_time=info.date_time)
                zi.compress_type = info.compress_type
                zi.external_attr = info.external_attr
                dst.writestr(zi, data)
            if not replaced:
                dst.writestr(member, payload)
    shutil.move(tmp, archive)
    return replaced


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    member, source, archives = argv[0], argv[1], argv[2:]
    payload = open(source, "rb").read()
    for a in archives:
        if not os.path.exists(a):
            print("  missing: {}".format(a))
            return 1
        before = os.path.getsize(a)
        existed = update(a, member, payload)
        after = os.path.getsize(a)
        print("  {}: {} {} ({:,} -> {:,} bytes)".format(
            os.path.basename(a), "replaced" if existed else "added", member,
            before, after))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
