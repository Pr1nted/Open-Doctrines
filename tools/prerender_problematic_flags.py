#!/usr/bin/env python3
"""Pre-render SVG flags to PNG using rsvg-convert with aspect ratio preservation.

All flags are pre-rendered to 256x128 PNGs with the flag content centered and
at its natural aspect ratio (transparent padding for non-2:1 flags). This fixes:
  - Clipping for non-2:1 flags (Venezuela 3:2, Mauritania 3:2, Iran 7:4, Nepal 5:6, etc.)
  - Stretching (all flags now preserve their natural aspect ratio)
  - nanosvg compatibility issues (Venezuela, Iran complex transforms)
  - Missing blue border on Nepal (added separately to NPL.svg)

Usage:
    python3 tools/prerender_problematic_flags.py            # pre-render all flags
    python3 tools/prerender_problematic_flags.py --problematic-only  # only clipPath/negative viewBox (legacy)
"""

import json, os, subprocess, sys, argparse

DATA_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")
FLAGS_DIR = os.path.join(DATA_DIR, "flags")
COUNTRIES_JSON = os.path.join(DATA_DIR, "countries.json")

# Known nanosvg-unfriendly features (used for --problematic-only mode)
PROBLEM_MARKERS = [
    "clipPath",
]


def has_problematic_features(svg_path: str) -> bool:
    """Check if SVG has features that nanosvg can't handle (legacy detection)."""
    try:
        with open(svg_path, "r", errors="replace") as f:
            content = f.read()
    except Exception:
        return True
    for marker in PROBLEM_MARKERS:
        if marker in content:
            return True
    if "viewBox" in content:
        import re
        vb_match = re.search(r'viewBox\s*=\s*"([^"]+)"', content)
        if vb_match:
            parts = vb_match.group(1).split()
            if len(parts) == 4:
                try:
                    x, y = float(parts[0]), float(parts[1])
                    if x < 0 or y < 0:
                        return True
                except ValueError:
                    pass
    return False


def prerender_flag(svg_path: str, png_path: str) -> bool:
    """Pre-render a single SVG to PNG at 256x128 with preserved aspect ratio."""
    try:
        result = subprocess.run(
            ["rsvg-convert", svg_path,
             "--width", "256", "--height", "128",
             "--keep-aspect-ratio",
             "--page-width", "256", "--page-height", "128",
             "--format", "png",
             "--output", png_path],
            capture_output=True, timeout=30,
        )
        if result.returncode != 0:
            print(f"    rsvg-convert error: {result.stderr.decode()[:200]}")
            return False
        if not os.path.exists(png_path) or os.path.getsize(png_path) == 0:
            print(f"    FAILED (empty output)")
            return False
        return True
    except subprocess.TimeoutExpired:
        print(f"    TIMEOUT")
        return False
    except Exception as e:
        print(f"    ERROR: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(description="Pre-render SVG flags to PNG")
    parser.add_argument("--problematic-only", action="store_true",
                        help="Only pre-render flags with nanosvg-unfriendly features (legacy mode)")
    args = parser.parse_args()

    if not os.path.exists(COUNTRIES_JSON):
        print(f"ERROR: {COUNTRIES_JSON} not found")
        sys.exit(1)
    os.makedirs(FLAGS_DIR, exist_ok=True)

    # Check if rsvg-convert is available
    try:
        subprocess.run(["rsvg-convert", "--version"], capture_output=True)
    except FileNotFoundError:
        print("ERROR: rsvg-convert not found. Install librsvg:")
        print("  brew install librsvg")
        print("  apt install librsvg2-bin")
        sys.exit(1)

    with open(COUNTRIES_JSON) as f:
        countries = json.load(f)

    # Determine which SVGs to pre-render
    svgs_to_render = []
    for fname in sorted(os.listdir(FLAGS_DIR)):
        if not fname.endswith(".svg"):
            continue
        svg_path = os.path.join(FLAGS_DIR, fname)
        if args.problematic_only:
            if has_problematic_features(svg_path):
                svgs_to_render.append(fname)
        else:
            svgs_to_render.append(fname)

    mode_label = "problematic" if args.problematic_only else "all"
    print(f"Pre-rendering {len(svgs_to_render)} SVGs ({mode_label} mode) to PNG with aspect ratio preservation...")

    converted = 0
    failed = 0
    for fname in svgs_to_render:
        iso = fname.replace(".svg", "")
        svg_path = os.path.join(FLAGS_DIR, fname)
        png_name = f"{iso}.png"
        png_path = os.path.join(FLAGS_DIR, png_name)

        print(f"  {iso}: {fname} -> {png_name} ...", end=" ")
        sys.stdout.flush()
        if prerender_flag(svg_path, png_path):
            sz = os.path.getsize(png_path)
            print(f"OK ({sz} bytes)")
            converted += 1
        else:
            failed += 1

    # Update countries.json: point all flag_actual entries to .png
    updated = 0
    for cid, entry in countries.items():
        fa = entry.get("flag_actual", {})
        if isinstance(fa, dict):
            img = fa.get("image", "")
            if img.endswith(".svg"):
                iso = os.path.basename(img).replace(".svg", "")
                png_name = f"{iso}.png"
                png_path = os.path.join(FLAGS_DIR, png_name)
                if os.path.exists(png_path):
                    fa["image"] = f"flags/{png_name}"
                    updated += 1
        # Also update flag_censored to use .png if .png exists
        fc = entry.get("flag_censored", {})
        if isinstance(fc, dict):
            img = fc.get("image", "")
            if img.endswith(".svg"):
                iso = os.path.basename(img).replace(".svg", "")
                png_name = f"{iso}.png"
                png_path = os.path.join(FLAGS_DIR, png_name)
                if os.path.exists(png_path):
                    # Keep the censored flag as .svg if it relies on the censored=true flag,
                    # but if .png exists, point to .png (censored flag is the same image, just pixelated)
                    fc["image"] = f"flags/{png_name}"

    with open(COUNTRIES_JSON, "w") as f:
        json.dump(countries, f, indent=2)

    print(f"\nPre-rendered: {converted} PNGs, {failed} failures")
    print(f"Updated countries.json: {updated} entries now point to .png")
    print("Done")


if __name__ == "__main__":
    main()
