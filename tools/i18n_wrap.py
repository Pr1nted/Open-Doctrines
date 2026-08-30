#!/usr/bin/env python3
"""Wrap drawn string literals in T(), so they go through the language table.

Extracting the strings is only half of it. `DrawText("Play Singleplayer", ...)`
still draws the English literal however complete the German file is -- the call
has to ask. This does that edit mechanically, on the files it is given:

    DrawText("Back", x, y, s, c)      ->  DrawText(T("Back"), x, y, s, c)
    MeasureText("Back", s)            ->  MeasureText(T("Back"), s)

Only literals that are already keys in data/lang/en.json are touched, so a
filename or a format string that happens to sit in a drawing call is left
exactly as it was. Anything already wrapped is skipped, so this is safe to run
twice.

    python3 tools/i18n_wrap.py src/Game_Menus.cpp [more files...]
    python3 tools/i18n_wrap.py --dry src/Game_Menus.cpp
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CALLS = ("DrawText", "MeasureText")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry" in sys.argv
    if not args:
        print(__doc__)
        return 2

    en = json.load(open(os.path.join(ROOT, "data", "lang", "en.json"), encoding="utf-8"))

    # DrawText( "literal"  -- the literal must be the FIRST argument, because
    # that is the one that is drawn. A literal later in the call is a font name
    # or a fallback and none of this applies to it.
    pat = re.compile(r"\b(" + "|".join(CALLS) + r")\(\s*(\"(?:[^\"\\]|\\.)*\")")

    total = 0
    for path in args:
        text = open(path, encoding="utf-8").read()
        out_lines = []
        n = 0
        for line in text.split("\n"):
            stripped = line.lstrip()
            if stripped.startswith(("//", "*", "/*")):
                out_lines.append(line)
                continue

            def sub(m):
                nonlocal n
                lit = m.group(2)
                try:
                    value = json.loads(lit)
                except ValueError:
                    return m.group(0)
                if value not in en:
                    return m.group(0)
                n += 1
                return f"{m.group(1)}(T({lit})"

            out_lines.append(pat.sub(sub, line))
        if n and not dry:
            open(path, "w", encoding="utf-8").write("\n".join(out_lines))
        print(f"{'would wrap' if dry else 'wrapped'} {n:4d}  {os.path.relpath(path, ROOT)}")
        total += n
    print(f"{total} call(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
