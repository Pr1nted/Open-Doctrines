#!/usr/bin/env python3
"""Check a translated .oddlg against the English it was translated from.

A dialogue script is prose wrapped around machinery: page breaks, `::`
directives that point at UI targets and gate the tutorial, choice keys the game
branches on, and `{key=...}` ids that are looked up rather than printed. The
prose is meant to change. NONE of the machinery is, and when it does change the
failure is not a typo -- it is a tutorial that cannot be advanced, in one
language, on one page, which nobody finds until somebody plays it in Polish.

So this compares structure and ignores wording:

    python3 tools/dialog_check.py            # every language, every script
    python3 tools/dialog_check.py uk         # one language

What must match the English exactly:
  * the number of pages
  * every `::` directive line, in order and verbatim
  * every choice target (`-> key`), in order
  * every `{key=...}` id, in order
  * every `@Speaker` line, in order and verbatim

WHY SPEAKERS ARE NOT TRANSLATED. A speaker is an id. data/comms/cast.json
keys the portrait, the voice and the name plate on it, and says so in its own
notes: "the KEY stays the formal name because a .oddlg keys on it and an id
must not move". Translate `@Signals Officer` and the lookup misses -- no
portrait, the narrator's voice, and the raw id on the plate. What the player
reads is the `display` and `role` from cast.json, and those go through the
ordinary translation table like any other string.

What may differ freely: the prose, and nothing else.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIALOG = os.path.join(ROOT, "data", "dialog")


def skeleton(text):
    """The machinery of a script, with the prose thrown away."""
    pages, directives, choices, keys, speakers = 1, [], [], [], []
    for raw in text.split("\n"):
        line = raw.strip()
        if line.startswith("//"):
            continue
        if line == "---":
            pages += 1
        elif line.startswith("::"):
            directives.append(line)
        elif line.startswith(">"):
            # "> Label -> target"; the label is prose, the target is not.
            m = re.search(r"->\s*(\S+)\s*$", line)
            choices.append(m.group(1) if m else "(no target)")
        elif line.startswith("@"):
            speakers.append(line)
            keys.extend(re.findall(r"\{key=([^}]*)\}", raw))
    return {"pages": pages, "directives": directives,
            "choices": choices, "keys": keys, "speakers": speakers}


# THE EMPHASIS MARKERS COME IN PAIRS.
#
# §** ... **§ is how a line stresses a word. An odd number of § means one was
# dropped or doubled in translation, and the result is not a missing emphasis --
# it is the rest of the page rendered as though it were emphasised, or the
# marker itself printed. Cheap to check, invisible to spot by eye across twenty
# languages.
def markup_problems(text, label):
    out = []
    body = [l for l in text.split("\n") if not l.strip().startswith("//")]
    n = "\n".join(body).count("\u00a7")
    if n % 2:
        out.append(f"  {label}: {n} section markers -- they pair, so this is odd one out")
    return out


def compare(en, tr, label, problems):
    a, b = skeleton(en), skeleton(tr)
    if a["pages"] != b["pages"]:
        problems.append(f"  {label}: pages {b['pages']}, English has {a['pages']}")
    for field in ("directives", "choices", "keys", "speakers"):
        if a[field] != b[field]:
            problems.append(f"  {label}: {field} differ")
            for i, want in enumerate(a[field]):
                got = b[field][i] if i < len(b[field]) else "(missing)"
                if got != want:
                    problems.append(f"      [{i}] {got!r}\n          want {want!r}")
            for extra in b[field][len(a[field]):]:
                problems.append(f"      extra: {extra!r}")


DEV_ONLY = {"markup_demo.oddlg", "tutorial_pointer.oddlg"}


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    en_dir = os.path.join(DIALOG, "en")
    scripts = sorted(f for f in os.listdir(en_dir) if f.endswith(".oddlg"))
    # THE TEST CARDS ARE NOT CONTENT. markup_demo.oddlg and
    # tutorial_pointer.oddlg exist to exercise the textbox markup and the
    # pointer against a script that uses every feature once; only
    # Game_Screenshots.cpp opens them, and the tutorial walk skips them on
    # purpose. Counting them as untranslated left a permanent "40 not yet
    # translated" on a run where every line a player can reach was done.
    scripts = [s for s in scripts if s not in DEV_ONLY]

    langs = sorted(d for d in os.listdir(DIALOG)
                   if d != "en" and os.path.isdir(os.path.join(DIALOG, d)))
    if only:
        langs = [only]

    problems, checked, missing = [], 0, 0
    for lang in langs:
        for s in scripts:
            path = os.path.join(DIALOG, lang, s)
            if not os.path.exists(path):
                missing += 1          # falls back to English; not an error
                continue
            en = open(os.path.join(en_dir, s), encoding="utf-8").read()
            tr = open(path, encoding="utf-8").read()
            compare(en, tr, f"{lang}/{s}", problems)
            problems.extend(markup_problems(tr, f"{lang}/{s}"))
            checked += 1

    for p in problems:
        print(p)
    print(f"{checked} script(s) checked, {len(problems)} problem(s), "
          f"{missing} not yet translated (English is used for those)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
