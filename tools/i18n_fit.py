#!/usr/bin/env python3
"""Does every translation still fit the box English was measured for?

A widget sized for English is sized for the shortest language in the set, and
the failure is silent: the labels are CENTRED, so one wider than its button
does not clip -- it hangs out of both ends over whatever is beside it.
"Cancel Request Mutual Guarantee" did that in English, at 170px of label in a
154px button, and ninety-three translations did the same. That act is called
"Mutual Guarantee" now, and the rest were shortened or shrink at the draw.

Two halves, because neither can answer it alone:

  THE GAME knows the geometry. Run it with OD_I18N_FIT_MANIFEST=<path> and
  every box a screen drew records where it is, how wide, and at what size.

  THE GAME also knows the widths. A translation with non-Latin characters is
  measured against that language's glyph atlas, a pure-ASCII one goes to
  raylib's variable-width font, and Urdu goes through HarfBuzz -- three
  different answers, and an offline model of them was wrong by up to 17% on
  the first strings it was checked against. So this asks the game, through
  --measure-text, rather than guessing.

  THIS TOOL knows which strings can land in which box. That is a fact about
  the code, not the data, so it is written down in WIDGETS below.

    python3 tools/i18n_fit.py                     # measure and report
    python3 tools/i18n_fit.py --manifest p.tsv    # a manifest from elsewhere

Regenerate the manifest whenever a widget is added or resized:

    OD_I18N_FIT_MANIFEST=/tmp/manifest.tsv \\
        ./OpenDoctrines --screenshots /tmp/shots data/saves/Modern\\ Day.odsv
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LANG = os.path.join(ROOT, "data", "lang")
GAME = os.path.join(ROOT, "build", "OpenDoctrines.app", "Contents", "MacOS",
                    "OpenDoctrines")

CODES = ["uk", "be", "kk", "ja", "zh", "de", "it", "fr", "es", "cs", "sl", "sk",
         "pl", "af", "ar", "hi", "ko", "bg", "tr", "ur"]

# WHICH STRINGS CAN LAND IN WHICH BOX.
#
# The manifest records what a screen happened to draw; this records what it
# COULD draw. The diplomacy panel offers thirteen acts and the tour renders
# whichever four the test save's relations allow, so the other nine are only
# ever checked from here.
#
# "Cancel %s" wraps an act label and is always the longest form the button
# holds, so each act is checked wrapped as well as bare.
FLOOR = 9        # odText::fitToWidth's default floor size

ACTS = ["Call to Arms", "Break Alliance", "Mutual Guarantee",
        "Break Guarantee", "Request Alliance", "Request Ceasefire",
        "Break NAP", "Request NAP", "Request Guarantee", "Propose Trade",
        "Declare War"]

PANEL = ["Ceasefire Pending...", "Max level (10)", "Max level (5)",
         "Upgrade locked, research next industry", "Building... (%d turns)",
         "Research next level (max %d)", "Build Fort ($%.0f, 0/%dt)",
         "Upgrade Fort to level %d ($%.0f, 0/%dt)",
         "Upgrade to level %d ($%.0f, 0/%dt)", "Cancel (%s, $%.0f)",
         "Recruit %s ($%.0f)", "Cancel Orders (%d)", "Cancel Artillery (%d)",
         "Cancel scrap (%d)", "Building Port... (3 turns)",
         "Upgrade Port lv%d ($%.0f, 3t)", "Specialize (choose one) ($75, 3t)"]

# Keyed by (widget, box width) where one helper serves two panels at two
# sizes. drawActBtn draws BOTH the diplomacy acts, two to a row in a 154px
# box, and the province panel's build and recruit buttons, full width at
# 320px. Checking the wide ones against the narrow box reported twenty-odd
# overflows that cannot happen.
WIDGETS = {
    ("diplomacy act button", 154): ACTS + ["Cancel %s"],
    ("diplomacy act button", 320): PANEL,
    "confirm dialog button": ["Yes", "No", "Cancel", "Quit to Menu",
                              "Back to Menu", "Save", "Continue"],
    "panel title": ["Translated", "Not translated"],
    "browser history button": ["Turn History / Timelapse"],
    "browser dialog button": ["Open", "Delete", "Cancel", "Import", "Export"],
    "mp resolve-now button": ["Resolve now, without the rest"],
    "mp resolve-turn button": ["Resolve this turn now"],
    "mp seat button": ["Seat them"],
    "mp return-to-lobby button": ["Everyone to the lobby after this turn",
                                  "Stopping after this turn (press to cancel)"],
}

# Numbers stand in for the specifiers so a format string is measured at the
# length it is actually drawn at, not at the length of "%.0f".
SAMPLES = [("%.0f", "1250"), ("%.1f", "12.5"), ("%d", "10"), ("%zu", "10"),
           ("%s", "Request Ceasefire")]


def fill(s):
    for spec, value in SAMPLES:
        s = s.replace(spec, value)
    return s


def main():
    manifest = "/tmp/manifest.tsv"
    if "--manifest" in sys.argv:
        manifest = sys.argv[sys.argv.index("--manifest") + 1]
    if not os.path.exists(manifest):
        print(f"no manifest at {manifest} -- see the docstring for how to make one")
        return 2
    if not os.path.exists(GAME):
        print(f"no game binary at {GAME}")
        return 2

    # ── geometry: the narrowest box each widget was ever drawn at ──
    boxes = {}
    for line in open(manifest, encoding="utf-8"):
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 5:
            continue
        where, width, size = parts[0], int(parts[1]), int(parts[2])
        boxes[(where, size, width)] = True
    if not boxes:
        print("manifest is empty")
        return 2

    en = json.load(open(os.path.join(LANG, "en.json"), encoding="utf-8"))
    langs = {c: json.load(open(os.path.join(LANG, f"{c}.json"), encoding="utf-8"))
             for c in CODES}

    # ── jobs: every candidate, in every language, at every size it is drawn ──
    jobs, meta = [], {}
    unknown = set()
    for (where, size, width) in sorted(boxes):
        cands = WIDGETS.get((where, width), WIDGETS.get(where))
        if cands is None:
            unknown.add(f"{where} at {width}px")
            continue
        for key in cands:
            if key not in en:
                continue
            for code in ["en"] + CODES:
                value = key if code == "en" else langs[code].get(key)
                if not value:
                    continue
                texts = [value]
                # The cancel form wraps every act label.
                if key == "Cancel %s":
                    wrap = langs[code] if code != "en" else en
                    texts = [value.replace("%s", (wrap.get(a) or a)) for a in ACTS]
                for t in texts:
                    t = fill(t)
                    jobs.append((code, size, t))
                    meta[(code, size, t)] = (where, width, key)
                    # FLOOR is what fitToWidth will fall back to. Measuring
                    # there too is the difference between "renders a point
                    # smaller", which nobody notices, and "loses the end of
                    # the sentence", which is a bug.
                    jobs.append((code, FLOOR, t))
                    meta[(code, FLOOR, t)] = (where, width, key)

    jobs = sorted(set(jobs))
    inp, outp = "/tmp/i18n_fit_jobs.tsv", "/tmp/i18n_fit_out.tsv"
    with open(inp, "w", encoding="utf-8") as f:
        for code, size, text in jobs:
            f.write(f"{code}\t{size}\t{text}\n")

    print(f"measuring {len(jobs)} string(s) across {len(CODES) + 1} languages...")
    r = subprocess.run([GAME, "--measure-text", inp, outp],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(outp):
        print("the game could not measure:", r.stderr.strip()[:400])
        return 2

    at = {}
    for line in open(outp, encoding="utf-8"):
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 4:
            continue
        code, size, measured, text = parts[0], int(parts[1]), int(parts[2]), parts[3]
        at[(code, size, text)] = measured

    over, truncated = [], []
    for (code, size, text), measured in at.items():
        info = meta.get((code, size, text))
        if not info or size == FLOOR:
            continue
        where, width, key = info
        if measured <= width:
            continue
        floor_w = at.get((code, FLOOR, text), measured)
        row = (measured - width, measured, width, code, where, text)
        (truncated if floor_w > width else over).append(row)

    truncated.sort(reverse=True)
    over.sort(reverse=True)
    if truncated:
        print("LOSES TEXT -- too wide even at the floor size, so it is cut:")
        for excess, measured, width, code, where, text in truncated:
            print(f"  +{excess:4}px  {code:3} {where} ({measured} in {width})  {text!r}")
        print()
    print("shrinks to fit -- renders a point or two smaller, nothing lost:")
    for excess, measured, width, code, where, text in over[:12]:
        print(f"  +{excess:4}px  {code:3} {where} ({measured} in {width})  {text!r}")
    if len(over) > 12:
        print(f"  ... and {len(over) - 12} more")
    if unknown:
        print("\nno candidate list for: " + ", ".join(sorted(unknown))
              + "\n  (add it to WIDGETS, or those boxes go unchecked)")
    print(f"\n{len(truncated)} label(s) lose text, {len(over)} shrink to fit, "
          f"across {len({o[4] for o in over + truncated})} widget(s)")
    return 1 if truncated else 0


if __name__ == "__main__":
    sys.exit(main())
