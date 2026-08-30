#!/usr/bin/env python3
"""Merge a batch of translations into one language file.

    python3 tools/i18n_merge.py be batch.json

The batch is {english: translation}. Anything already translated is left
alone, and a key that is not in en.json is reported rather than written --
a translation of a string the game no longer draws is dead weight, and one
of a MISREMEMBERED string is worse, because it looks like coverage.
"""

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG = os.path.join(ROOT, "data", "lang")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    code, batch_path = sys.argv[1], sys.argv[2]
    en = json.load(open(os.path.join(LANG, "en.json"), encoding="utf-8"))
    path = os.path.join(LANG, f"{code}.json")
    cur = json.load(open(path, encoding="utf-8"))
    batch = json.load(open(batch_path, encoding="utf-8"))

    unknown = [k for k in batch if k not in en]
    added = 0
    for k, v in batch.items():
        if k in en and v:
            cur[k] = v
            added += 1
    out = {k: cur.get(k, "") for k in en}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1, sort_keys=True)
        f.write("\n")

    done = sum(1 for v in out.values() if v)
    print(f"{code}: +{added} -> {done}/{len(en)} ({100.0*done/len(en):.1f}%)")
    for k in unknown:
        print(f"  NOT IN en.json: {k!r}")
    return 1 if unknown else 0


if __name__ == "__main__":
    sys.exit(main())
