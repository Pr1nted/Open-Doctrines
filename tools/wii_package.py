#!/usr/bin/env python3
"""Build, or check, the Wii homebrew zip.

    tools/wii_package.py --dol build/OpenDoctrines.dol --stage dist/stage --out dist/OpenDoctrines-wii-ppc.zip
    tools/wii_package.py --check dist/OpenDoctrines-wii-ppc.zip

The Homebrew Channel lists every folder under sd:/apps/ that holds a boot.dol,
and shows it with the name and icon beside it. A player unzips this onto the
root of their SD card, so the zip IS that layout:

    apps/opendoctrines/boot.dol    the game (CMakeLists.txt, "THE NINTENDO WII")
    apps/opendoctrines/meta.xml    name, version, description for the channel
    apps/opendoctrines/icon.png    128x48, the channel's icon
    apps/opendoctrines/data/       the shipped data, exactly what tools/package.py
                                   allowlists for every other platform (--stage
                                   is its output); the game finds it beside its
                                   executable, which the channel passes as argv[0]
    apps/opendoctrines/READ ME FIRST.txt, VERSION   from package.py too

THE ICON IS MADE HERE from data/Icon/icon.png rather than committed, for the
reason tools/vita_vpk.py gives: a second copy in another size is a copy that
drifts.

--check opens a built zip and holds it to that shape -- the DOL header, the
icon's size, meta.xml's fields, the fonts in data/ -- because there is no Wii on
a CI runner to try installing it on.
"""
import argparse
import io
import os
import struct
import sys
import time
import zipfile
from xml.etree import ElementTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import odver

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = "apps/opendoctrines/"
ICON_SIZE = (128, 48)
BG = (16, 20, 28)      # the website's dark background


def make_icon():
    from PIL import Image
    src = Image.open(os.path.join(ROOT, "data", "Icon", "icon.png")).convert("RGBA")
    icon = Image.new("RGBA", ICON_SIZE, BG + (255,))
    side = ICON_SIZE[1] - 4
    mark = src.resize((side, side), Image.LANCZOS)
    icon.alpha_composite(mark, ((ICON_SIZE[0] - side) // 2, 2))
    out = io.BytesIO()
    icon.save(out, "PNG")
    return out.getvalue()


def make_meta(version):
    return (
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
        '<app version="1">\n'
        '  <name>Open Doctrines</name>\n'
        '  <coder>Open Doctrines contributors</coder>\n'
        f'  <version>{version}</version>\n'
        f'  <release_date>{time.strftime("%Y%m%d%H%M%S", time.gmtime())}</release_date>\n'
        '  <short_description>Grand strategy, 1641 provinces</short_description>\n'
        '  <long_description>A free grand strategy game. Point with the Wii Remote: '
        'A clicks, B is the right button, + and - zoom, 1 is back. A USB keyboard '
        'works for typing. Worlds load at reduced resolution to fit the Wii\'s memory; '
        'single player, no sound yet.</long_description>\n'
        '</app>\n'
    ).encode("utf-8")


def build(dol, stage, out):
    with open(dol, "rb") as f:
        dol_bytes = f.read()
    problems = check_dol(dol_bytes)
    if problems:
        print("FAILED: " + problems, file=sys.stderr)
        return 1
    version = str(odver.read())
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(APP + "boot.dol", dol_bytes)
        z.writestr(APP + "meta.xml", make_meta(version))
        z.writestr(APP + "icon.png", make_icon())
        for dirpath, _, files in os.walk(stage):
            for name in sorted(files):
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, stage).replace(os.sep, "/")
                # package.py copies the binary into the stage too; the zip
                # carries it once, as boot.dol.
                if rel in ("OpenDoctrines", "OpenDoctrines.elf", "OpenDoctrines.dol"):
                    continue
                z.write(full, APP + rel)
    print(f"wrote {out} ({os.path.getsize(out) // 1024} KB), Open Doctrines {version}")
    return check(out)


def check_dol(b):
    # A DOL is a 0x100-byte header of big-endian words: 7 text and 11 data
    # section offsets, their load addresses and sizes, the BSS, the entry.
    if len(b) < 0x100:
        return "boot.dol is shorter than a DOL header"
    entry = struct.unpack(">I", b[0xE0:0xE4])[0]
    text0 = struct.unpack(">I", b[0x48:0x4C])[0]
    if not (0x80000000 <= entry < 0x81800000):
        return f"boot.dol entry point 0x{entry:08x} is not in the Wii's MEM1"
    if not (0x80000000 <= text0 < 0x81800000):
        return f"boot.dol first text section loads at 0x{text0:08x}, not in MEM1"
    return ""


def check(path):
    problems = []
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
        for need in ("boot.dol", "meta.xml", "icon.png", "VERSION"):
            if APP + need not in names:
                problems.append(f"no {APP}{need}")
        if APP + "boot.dol" in names:
            p = check_dol(z.read(APP + "boot.dol"))
            if p:
                problems.append(p)
        if APP + "meta.xml" in names:
            try:
                meta = ElementTree.fromstring(z.read(APP + "meta.xml"))
                for field in ("name", "version", "short_description"):
                    if not (meta.findtext(field) or "").strip():
                        problems.append(f"meta.xml has no {field}")
            except ElementTree.ParseError as e:
                problems.append(f"meta.xml does not parse: {e}")
        if APP + "icon.png" in names:
            from PIL import Image
            size = Image.open(io.BytesIO(z.read(APP + "icon.png"))).size
            if size != ICON_SIZE:
                problems.append(f"icon.png is {size}, the channel wants {ICON_SIZE}")
        if not any(n.startswith(APP + "data/fonts/") for n in names):
            problems.append("no data/fonts/: the game would start with no text")
        if any(not n.startswith(APP) for n in names):
            problems.append("something outside apps/opendoctrines/")
    if problems:
        print("FAILED: " + "; ".join(problems), file=sys.stderr)
        return 1
    print(f"ok    {path}: Homebrew Channel layout, DOL header, meta.xml, 128x48 icon, data")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dol")
    ap.add_argument("--stage")
    ap.add_argument("--out")
    ap.add_argument("--check", metavar="ZIP")
    a = ap.parse_args()
    if a.check:
        return check(a.check)
    if not (a.dol and a.stage and a.out):
        ap.error("--dol, --stage and --out, or --check")
    return build(a.dol, a.stage, a.out)


if __name__ == "__main__":
    sys.exit(main())
