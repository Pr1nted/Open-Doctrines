#!/usr/bin/env python3
"""Merge translations into one language file.

    python3 tools/i18n_put.py de < batch.json

Reads {"English string": "translation", ...} on stdin and writes it into
data/lang/<code>.json. Refuses a key that is not in en.json, because a key the
game never draws is a translation nobody will ever see and usually means the
English side was retyped rather than copied.

Existing translations are replaced; anything not mentioned is left alone, so a
batch is additive and can be run again.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LANG = os.path.join(ROOT, "data", "lang")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    code = sys.argv[1]
    path = os.path.join(LANG, f"{code}.json")
    if not os.path.exists(path):
        print(f"no such language file: {path}")
        return 1

    en = json.load(open(os.path.join(LANG, "en.json"), encoding="utf-8"))
    have = json.load(open(path, encoding="utf-8"))
    batch = json.load(sys.stdin)

    unknown = [k for k in batch if k not in en]
    if unknown:
        print(f"{len(unknown)} key(s) are not strings the game draws:")
        for k in unknown[:10]:
            print(f"  {k!r}")
        return 1

    changed = 0
    for k, v in batch.items():
        if v and have.get(k) != v:
            have[k] = v
            changed += 1
    with open(path, "w", encoding="utf-8") as f:
        json.dump(have, f, ensure_ascii=False, indent=1, sort_keys=True)
        f.write("\n")
    done = sum(1 for k in en if have.get(k))
    print(f"{code}: +{changed}, now {done}/{len(en)} ({100.0 * done / len(en):.0f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
