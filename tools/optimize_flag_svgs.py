#!/usr/bin/env python3
"""Shrink the flag SVGs as far as each one can survive, and no further.

    python3 tools/optimize_flag_svgs.py                 # optimise data/flags
    python3 tools/optimize_flag_svgs.py --dry-run       # report, write nothing

WHY

data/flags carries ~11 MB of SVG for artwork the game rasterises at 256x128.
Most of it is CorelDRAW export: coordinate spaces hundreds of thousands of units
wide, five decimal places on every number, editor metadata, and one <style>
block per file where a presentation attribute would do.

WHY NOT JUST RUN SVGO OVER IT

Because the renderer is nanosvg, which is not a complete SVG implementation, and
an optimiser is free to emit valid SVG that it draws differently or not at all.
Measured: precision 0 is safe for DOM.svg (6% of the original) and visibly wrong
for AFG.svg. One global setting therefore either breaks files or leaves most of
the saving on the table.

So every candidate is RENDERED THROUGH NANOSVG and compared with the reference,
and each file keeps the most aggressive precision it actually survives. The
checker is tests/svg_render_diff.cpp; build it with

    cmake --build build --target SvgRenderDiff

THE <style> FILES ARE DIFFERENT, AND THE DIFFERENCE IS A BUG BEING FIXED

A dozen of these files put their fills in a <style> block and reference them
with class=. nanosvg does not apply CSS, so it draws those shapes with the
default fill -- SRB_KINGDOM.svg, the Kingdom of Serbia, renders in-game as a
BLACK field with a coat of arms on it instead of a red-blue-white tricolour.
svgo's inlineStyles turns those rules into presentation attributes, which
nanosvg does read, so optimising them changes what is drawn: it starts being
right.

That means the original cannot be the reference for those files -- it is the
broken picture. They are compared against a near-lossless optimisation of
themselves instead (precision 4, styles already inlined), which validates the
precision choice without treating the bug as the target.

Most <style> files turn out to draw the same either way, so the report asks the
RENDERER which ones actually changed rather than assuming all of them did.
Measured on this artwork: twelve files carry CSS, three of them were drawing
wrong (BLZ, BTN, SRB_KINGDOM), and those three are listed on their own because
they are the only ones whose appearance in-game moves.

SVGO IS NEEDED ONLY TO RUN THIS, NEVER TO BUILD THE GAME

The output is committed. Neither CI nor CMake ever invokes svgo or node.
"""

import argparse
import os
import shutil
import subprocess
import sys

FLAG_DIR = "data/flags"
DIFFER = "build/SvgRenderDiff"

# Tried in this order; the first that renders correctly wins. Below 0 there is
# nothing left to round.
PRECISIONS = [0, 1, 2, 3]

# The reference precision for files whose original does not render correctly.
# High enough that rounding is not the variable being tested.
REFERENCE_PRECISION = 4


def svgo_command(explicit):
    """How to invoke svgo, preferring anything that is not npx.

    npx re-resolves the package on every call, which is fine once and hopeless
    a thousand times -- and a thousand is what per-file processing costs.
    """
    if explicit:
        return [explicit]
    on_path = shutil.which("svgo")
    if on_path:
        return [on_path]
    print("svgo not found; falling back to npx (slow). Install it once with:",
          file=sys.stderr)
    print("    npm install --prefix build/svgo-tool svgo@3", file=sys.stderr)
    return ["npx", "--yes", "svgo@3"]


def run_svgo(cmd, src, dst, precision):
    """One file. Returns True if svgo produced output.

    PER FILE, NOT PER DIRECTORY, because svgo dies on malformed path data --
    measured: a smooth-curve command with no preceding curve crashes
    convertPathData's reflectPoint. In directory mode that one file takes the
    whole run down and 247 good ones are lost with it. Here it costs exactly
    the file it happened to, which then keeps its original.
    """
    try:
        r = subprocess.run(cmd + ["--multipass", "-p", str(precision),
                                  "-i", src, "-o", dst],
                           capture_output=True, text=True, timeout=120)
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return False
    return r.returncode == 0 and os.path.isfile(dst)


def renders_same(differ, before, after, size):
    """True when nanosvg draws `after` the same as `before`, at flag size.

    AT THE SIZE THE GAME DRAWS, which is the whole point. Every call site
    rasterises a flag at 256x128 (Game_Loading, Game_TurnLogic) or 240x120 (the
    map editor), and a texture is scaled on the GPU after that rather than
    re-rasterised -- so precision beyond 256x128 buys nothing a player can see.
    Checked at 512x256 out of curiosity, 55 files show edge pixels moving by an
    antialiasing step; tightening for that would cost real megabytes to fix a
    rendering nobody performs. If a call site ever asks for something bigger,
    re-run this with --check-size and the affected files will re-optimise less
    aggressively on their own.
    """
    r = subprocess.run([differ, before, after, str(size[0]), str(size[1])],
                       capture_output=True, text=True)
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=FLAG_DIR)
    ap.add_argument("--differ", default=DIFFER)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--work", default="build/flag-opt")
    ap.add_argument("--svgo", default=None,
                    help="path to the svgo binary (default: svgo on PATH, else npx)")
    ap.add_argument("--check-size", default="256x128",
                    help="size the render check rasterises at; the largest any "
                         "call site asks for (default 256x128)")
    args = ap.parse_args()

    try:
        cw, ch = (int(v) for v in args.check_size.lower().split("x"))
    except ValueError:
        sys.exit(f"--check-size wants WxH, got {args.check_size!r}")
    check = (cw, ch)

    if not os.path.isfile(args.differ):
        sys.exit(f"{args.differ} not found. Build it first:\n"
                 f"    cmake --build build --target SvgRenderDiff")

    names = sorted(f for f in os.listdir(args.dir) if f.endswith(".svg"))
    if not names:
        sys.exit(f"no .svg in {args.dir}")

    cmd = svgo_command(args.svgo)
    for p in PRECISIONS + [REFERENCE_PRECISION]:
        os.makedirs(os.path.join(args.work, f"p{p}"), exist_ok=True)

    print(f"optimising {len(names)} flags, trying precision "
          f"{PRECISIONS[0]}..{PRECISIONS[-1]} on each, "
          f"checked at {cw}x{ch}...")

    styled, shrunk, minimal, rejected, broke = [], [], [], [], []
    before_total = after_total = 0

    for i, name in enumerate(names, 1):
        src = os.path.join(args.dir, name)
        orig_size = os.path.getsize(src)
        before_total += orig_size
        if i % 25 == 0:
            print(f"  ...{i}/{len(names)}")

        with open(src, encoding="utf-8", errors="replace") as fh:
            has_style = "<style" in fh.read()

        # The reference this file's candidates must match. See the module
        # docstring for why a <style> file cannot be its own reference.
        reference = src
        if has_style:
            ref = os.path.join(args.work, f"p{REFERENCE_PRECISION}", name)
            if run_svgo(cmd, src, ref, REFERENCE_PRECISION):
                reference = ref

        chosen = None
        produced_any = False       # svgo ran at all
        produced_smaller = False   # ...and got anywhere
        for p in PRECISIONS:
            c = os.path.join(args.work, f"p{p}", name)
            if not run_svgo(cmd, src, c, p):
                continue
            produced_any = True
            if os.path.getsize(c) >= orig_size:
                continue
            produced_smaller = True
            if renders_same(args.differ, reference, c, check):
                chosen = (p, c)
                break

        if chosen is None:
            after_total += orig_size
            # Three different things, and lumping them together hides the only
            # one worth looking at. A file svgo cannot smallen is finished; a
            # file it CAN smallen but only by changing the picture is a file
            # where the checker earned its keep and somebody may want to look.
            if not produced_any:
                broke.append(name)
            elif not produced_smaller:
                minimal.append((name, orig_size))
            else:
                rejected.append((name, orig_size))
            continue

        p, c = chosen
        new_size = os.path.getsize(c)
        after_total += new_size

        # Did the PICTURE change? Only a <style> file can have got here with a
        # different one, but not every <style> file does -- most of them put
        # something in CSS that nanosvg was going to draw the same way anyway.
        # Classifying by "contains <style>" reported twelve changed flags when
        # three had changed, so ask the renderer instead of the file.
        changed = has_style and not renders_same(args.differ, src, c, check)

        if not args.dry_run:
            shutil.copyfile(c, src)
        (styled if changed else shrunk).append((name, orig_size, new_size, p))

    def mb(n):
        return n / 1048576.0

    print()
    for name in sorted(broke):
        print(f"  svgo could not process, left untouched: {name}")
    if minimal:
        print(f"  {len(minimal)} already minimal, nothing to gain "
              f"({sum(s for _, s in minimal)} bytes in total)")
    if rejected:
        print("  REJECTED by the render check -- smaller, but it changed the")
        print("  picture, so the original was kept:")
        for name, size in sorted(rejected):
            print(f"    {name:<28} {size:>8} bytes")
    if styled:
        print()
        print("  APPEARANCE CHANGED -- these had CSS fills nanosvg was ignoring,")
        print("  so they were drawing WRONG in-game and now draw correctly.")
        print("  Verified by eye, not just by the numbers:")
        for name, a, b, p in sorted(styled):
            print(f"    {name:<28} {a:>8} -> {b:>7}  (precision {p})")

    print()
    print(f"  {len(shrunk) + len(styled)} of {len(names)} optimised, "
          f"{len(minimal) + len(rejected) + len(broke)} left alone")
    print(f"  {mb(before_total):.2f} MB -> {mb(after_total):.2f} MB "
          f"({100.0 * after_total / before_total:.1f}%, "
          f"{mb(before_total - after_total):.2f} MB saved)")
    if args.dry_run:
        print("  --dry-run: nothing was written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
