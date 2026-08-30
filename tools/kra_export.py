#!/usr/bin/env python3
"""Export a Krita file with its background layer hidden.

    python3 tools/kra_export.py ~/Desktop/Images/Mia_concept.kra out.png
    python3 tools/kra_export.py mia.kra --list
    python3 tools/kra_export.py mia.kra out.png --hide Background --hide 10 --hide 3

WHY. A character drawn on a white background has to be separated from it
before the transmission window can use it, and keying is the wrong tool for a
job the file can answer directly: the .kra HAS a background layer, so the
honest move is to turn it off and let Krita composite the rest.

Keying leaves three problems this avoids entirely:

  - a pale fringe along every edge, because the antialiased boundary pixels
    are already part-background and cannot be unmixed
  - gaps a flood cannot reach. A figure with a hand on its hip encloses a
    triangle of sky between arm and body, and a flood that starts at the
    border never gets in
  - and then, fixing that, a size threshold to tell an arm gap from the white
    of an eye -- which is a guess that gets it wrong on the next character

The same switch does a second job. The eyes and the mouth are the parts the
window ANIMATES, so they must not be in the picture underneath -- and painting
them out afterwards means guessing at a radius and smearing skin over the
artist's line. Turning their layers off removes them exactly, because the file
already knows where they are. --list prints the layers so they can be found.

Layers can be named by name or by INDEX, and index is not a convenience: a
Krita file happily holds two layers both called "Paint Layer 5", and this
character's file does. Hiding by name would take both.

A .kra is a zip. This copies it with those layers switched off in maindoc.xml
and hands the copy to Krita's own exporter, so the compositing is done by the
program that drew it.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

KRITA_CANDIDATES = [
    "/Applications/krita.app/Contents/MacOS/krita",
    "/usr/bin/krita",
    "/usr/local/bin/krita",
    "krita",
]


def find_krita():
    for c in KRITA_CANDIDATES:
        if os.path.exists(c):
            return c
    from shutil import which
    found = which("krita")
    if found:
        return found
    sys.exit("krita not found -- install it, or export the PNG by hand with the "
             "background layer hidden")


def read_layers(src):
    """Every <layer> tag in document order, with its span and its name."""
    zin = zipfile.ZipFile(src)
    doc = zin.read("maindoc.xml").decode()
    zin.close()
    spans = [m.span() for m in re.finditer(r"<layer\b[^>]*?/?>", doc)]
    names = [re.search(r'\bname="([^"]*)"', doc[a:b]).group(1) for a, b in spans]
    return doc, spans, names


def hide_layers(src, dst, wanted):
    """Copy the .kra with the named or numbered layers switched off."""
    doc, spans, names = read_layers(src)

    idx = set()
    for w in wanted:
        if w.isdigit():
            i = int(w)
            if not 0 <= i < len(names):
                sys.exit(f"no layer {i}; the file has {len(names)}. Use --list.")
            idx.add(i)
            continue
        hits = [i for i, n in enumerate(names) if n == w]
        if not hits:
            sys.exit(f'no layer named "{w}". Use --list to see them.')
        idx.update(hits)

    # Back to front, so the spans of the tags not yet rewritten stay valid.
    out = doc
    for i in sorted(idx, reverse=True):
        a, b = spans[i]
        out = out[:a] + re.sub(r'\bvisible="[^"]*"', 'visible="0"', doc[a:b]) + out[b:]

    # Read the archive ONCE, up front. ZipFile.writestr(zinfo, ...) MUTATES
    # the ZipInfo it is given -- it stamps a header_offset for the new archive
    # -- so entries taken from the source's infolist() cannot be reused for a
    # later read; the next one fails with "Bad magic number for file header".
    zin = zipfile.ZipFile(src)
    entries = [(it.filename, it.compress_type, zin.read(it.filename)) for it in zin.infolist()]
    zin.close()
    blob = {n: d for n, _, d in entries}

    zout = zipfile.ZipFile(dst, "w")
    # mimetype has to come first and be stored, or the file is not a .kra.
    zout.writestr(zipfile.ZipInfo("mimetype"), blob["mimetype"], zipfile.ZIP_STORED)
    for name, ctype, data in entries:
        if name == "mimetype":
            continue
        zout.writestr(zipfile.ZipInfo(name),
                      out.encode() if name == "maindoc.xml" else data, ctype)
    zout.close()
    return sorted(idx), names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--hide", action="append", default=None, metavar="NAME_OR_INDEX",
                    help="a layer to switch off, by name or by index; repeatable. "
                         "Defaults to Background alone.")
    ap.add_argument("--layer", help="deprecated spelling of --hide")
    ap.add_argument("--only", action="append", default=None, metavar="NAME_OR_INDEX",
                    help="keep ONLY these layers and switch every other one off; "
                         "repeatable. For lifting one part of a drawing out as its "
                         "own sprite.")
    ap.add_argument("--list", action="store_true", help="print the layers and stop")
    args = ap.parse_args()

    if args.list:
        _, _, names = read_layers(args.source)
        for i, n in enumerate(names):
            print(f"  [{i:2}] {n}")
        return
    if not args.out:
        ap.error("an output path is required unless --list is given")

    if args.only:
        _, _, names = read_layers(args.source)
        keep = set()
        for w in args.only:
            if w.isdigit():
                keep.add(int(w))
            else:
                keep.update(i for i, n in enumerate(names) if n == w)
        if not keep:
            sys.exit("--only matched no layers. Use --list.")
        wanted = [str(i) for i in range(len(names)) if i not in keep]
    else:
        wanted = list(args.hide or [])
        if args.layer:
            wanted.append(args.layer)
        if not wanted:
            wanted = ["Background"]

    krita = find_krita()
    with tempfile.TemporaryDirectory() as tmp:
        staged = os.path.join(tmp, "nobg.kra")
        idx, names = hide_layers(args.source, staged, wanted)
        print("  hid " + ", ".join(f"[{i}] {names[i]!r}" for i in idx))
        # Krita prints a tile-leak warning on exit that means nothing here.
        subprocess.run([krita, "--export", "--export-filename", args.out, staged],
                       check=True, capture_output=True)

    from PIL import Image
    import numpy as np
    im = Image.open(args.out).convert("RGBA")
    a = np.asarray(im)
    clear = int((a[:, :, 3] < 20).sum())
    print(f"  wrote {args.out}  {im.size[0]}x{im.size[1]}  "
          f"{100.0 * clear / (im.size[0] * im.size[1]):.0f}% transparent")
    if clear == 0:
        print("  WARNING: nothing came out transparent. Is the background layer "
              "really called what you think it is?")


if __name__ == "__main__":
    main()
