#!/usr/bin/env python3
"""Can the web build draw the name of every language it offers?

THE PICKER IS THE ONE SCREEN THAT MUST BE READABLE BEFORE YOU CHOOSE. The web
build preloads a SUBSET of unifont (tools/subset_font.py) and fetches the whole
11 MB font only once a language needs glyphs the subset has not got -- which
cannot help the list you are choosing from. Japanese, Chinese, Korean, Arabic,
Hindi, Georgian and Armenian were blank rows, and Vietnamese and Azerbaijani
were missing single letters, until the subset was told to keep the names too.

So this builds the subset the web build would ship and asserts that every
endonym in src/i18n/Locale.cpp can be drawn from it. Needs fontTools; without
it the check skips, exactly as the build does.

Usage:  python3 tools/check_web_font.py
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    src = os.path.join(ROOT, "data", "fonts", "unifont.ttf")
    if not os.path.exists(src):
        print("skip: no data/fonts/unifont.ttf here")
        return 0
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        print("skip: fontTools is not installed, which is also what the web "
              "build does (it then ships the whole font)")
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "subset.ttf")
        rc = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "subset_font.py"),
                             src, out], capture_output=True, text=True)
        if rc.returncode != 0 or not os.path.exists(out):
            print("FAILED: the subsetter did not run:", rc.stderr.strip())
            return 1
        cmap = TTFont(out).getBestCmap()

    locale = open(os.path.join(ROOT, "src", "i18n", "Locale.cpp"), encoding="utf-8").read()
    rows = re.findall(r'\{\s*"([a-z]{2})"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"', locale)
    if len(rows) < 10:
        print("FAILED: could not read the language table")
        return 1

    bad = []
    for code, endonym, english in rows:
        missing = [c for c in endonym if ord(c) not in cmap]
        if missing:
            bad.append((code, english, endonym, "".join(missing)))

    if bad:
        print("FAILED: the web subset cannot draw these languages' own names:")
        for code, english, endonym, missing in bad:
            print(f"    {code}  {english:12} {endonym}   missing: {missing}")
        print("  Add what they need to BLOCKS in tools/subset_font.py, or keep the")
        print("  endonym codepoints it already collects.")
        return 1

    print(f"ok    the web subset draws all {len(rows)} language names")
    return 0


if __name__ == "__main__":
    sys.exit(main())
