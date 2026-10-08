#!/usr/bin/env python3
"""Measure how much of its own canvas a symbol SVG covers, and reframe it.

WHY THIS SHELLS OUT INSTEAD OF PARSING PATHS

The only bounding box worth having is the one the game gets, and the game
rasterises with nanosvg, which is not a conformant renderer: it ignores <use>,
gradients and clip paths, and flattens arcs its own way. A box computed from
path data in Python would answer a different question -- and would answer it
differently again the day either side changed. tools/symbol_fill.cpp links the
same header src/renderer/FlagRenderer.cpp does, so what it reports is what a
flag will show.

Build it with the game (target SymbolFill) or let this module compile it on
demand; it is one self-contained file over a header-only dependency.
"""

import json
import os
import shutil
import subprocess
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(TOOLS_DIR)
SOURCE = os.path.join(TOOLS_DIR, "symbol_fill.cpp")
INCLUDE = os.path.join(PROJECT_ROOT, "src", "renderer")
CACHED_BINARY = os.path.join(PROJECT_ROOT, "build", "symbol-fill")

# The share of its own canvas a symbol's artwork should span. Not 100%: a
# symbol that touches its own edge has no breathing room when it lands on a
# flag, and several already do. 92% is where the majority of data/symbols/
# sits, so it is the convention rather than a new opinion.
TARGET_FILL = 0.92


class Unavailable(RuntimeError):
    """symbol-fill could not be found or built."""


def _candidates():
    yield CACHED_BINARY
    for sub in ("", "macos", "linux", "windows"):
        yield os.path.join(PROJECT_ROOT, "build", sub, "symbol-fill")
        yield os.path.join(PROJECT_ROOT, "build", sub, "symbol-fill.exe")


def binary(build_if_missing=True):
    """Path to a usable symbol-fill, building it if that is allowed."""
    for path in _candidates():
        if os.path.isfile(path) and os.access(path, os.X_OK):
            if os.path.getmtime(path) >= os.path.getmtime(SOURCE):
                return path
    if not build_if_missing:
        raise Unavailable("symbol-fill is not built (cmake target SymbolFill)")

    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        raise Unavailable("no C++ compiler found to build symbol-fill; "
                          "build the cmake target SymbolFill instead")
    os.makedirs(os.path.dirname(CACHED_BINARY), exist_ok=True)
    cmd = [cxx, "-O2", "-std=c++17", "-I", INCLUDE, "-o", CACHED_BINARY, SOURCE]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise Unavailable(f"could not build symbol-fill:\n{proc.stderr.strip()}")
    return CACHED_BINARY


def measure(paths, build_if_missing=True):
    """{path: {"bbox": (x0,y0,x1,y1), "fill": f, "ink": i}} for each SVG.

    bbox is a fraction of the file's own canvas, so 0..1 spans the viewBox.
    Files that fail to parse, or that rasterise to nothing, are reported with
    an "error" key rather than dropped -- a symbol that draws nothing is the
    loudest possible failure and must not look like a missing dict entry.
    """
    paths = list(paths)
    if not paths:
        return {}
    out = subprocess.run([binary(build_if_missing), "--json"] + paths,
                         capture_output=True, text=True)
    results = {}
    for line in out.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        rec = json.loads(line)
        path = rec.pop("file")
        if "bbox" in rec:
            rec["bbox"] = tuple(rec["bbox"])
        results[path] = rec
    if not results and out.returncode not in (0, 1):
        raise Unavailable(f"symbol-fill failed: {out.stderr.strip()}")
    return results


def correction(bbox, box=200.0, target=TARGET_FILL):
    """Extra scale and centre that would make `bbox` fill `target` of the box.

    Returns (k, cx, cy) in the user units of a viewBox `box` wide centred on
    the origin -- the canonical "-100 -100 200 200". Callers compose it with
    whatever transform they were already emitting:

        scale' = k * scale
        translate' = k * (translate - centre)

    which is that composition written out, not a separate convention.
    """
    x0, y0, x1, y1 = bbox
    half = box / 2.0
    u0, u1 = -half + box * x0, -half + box * x1
    v0, v1 = -half + box * y0, -half + box * y1
    span = max(u1 - u0, v1 - v0)
    if span <= 0:
        return 1.0, 0.0, 0.0
    return (box * target) / span, (u0 + u1) / 2.0, (v0 + v1) / 2.0


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if not args:
        args = [os.path.join(PROJECT_ROOT, "data", "symbols", f)
                for f in sorted(os.listdir(os.path.join(PROJECT_ROOT, "data", "symbols")))
                if f.endswith(".svg")]
    try:
        subprocess.run([binary()] + args, check=False)
    except Unavailable as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
