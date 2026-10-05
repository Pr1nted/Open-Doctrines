#!/usr/bin/env python3
"""Every OD_* instrument LOOP.md names must actually exist.

WHY THIS GUARD EXISTS (journal 527). `docs/ai/LOOP.md` is the document the next
iteration plans FROM, so a switch named there that no code reads is worse than a
missing one: somebody designs an experiment around it. This project has the
failure already -- journal 480 wrote a `[WIDTHPROBE]` that printed nothing
because it sat in a function gated off by default, the fourth instance of
"wiring checks are not execution".

SCOPE, deliberately narrow. Only LOOP.md. The journal and the backlog are
HISTORY and legitimately name gates that were built, measured and reverted
without ever being committed -- journal 527 verified three of them
(OD_PAC_COOL_LOSING, OD_RECRUIT_RESERVE, OD_RESEARCH_GUARD_OVERRUN) have zero
commits touching src/, which is the loop working as intended. Checking those
would be a guard that cries wolf, and a noisy check is worse than none.

A token counts as real if ANY source or tool file mentions it: harness gates
like OD_BENCH_SEEDS live in tools/*.py, not in src/ -- journal 527's first pass
reported 18 phantoms because it scanned src/ alone.
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Prose placeholders, not instruments: "OD_X=0 switches X OFF", "e.g. OD_SOMETHING_OFF".
PLACEHOLDERS = {"OD_X", "OD_SOMETHING_OFF"}
TOKEN = re.compile(r"\bOD_[A-Z0-9]+(?:_[A-Z0-9]+)*\b")


def main() -> int:
    loop = os.path.join(ROOT, "docs", "ai", "LOOP.md")
    if not os.path.exists(loop):
        print("check_ai_instruments: docs/ai/LOOP.md missing")
        return 1
    with open(loop, encoding="utf-8", errors="replace") as fh:
        named = {t for t in TOKEN.findall(fh.read())
                 if not t.endswith("_") and t not in PLACEHOLDERS}
    # Trailing-underscore tokens are wrapped or globbed prefixes, not names.
    haystack = subprocess.run(
        ["bash", "-c",
         "cat src/*.cpp src/*.h src/ai/*.cpp src/ai/*.h src/*/*.cpp src/*/*.h "
         "tools/*.py tools/*.sh 2>/dev/null"],
        cwd=ROOT, capture_output=True, text=True).stdout
    present = set(TOKEN.findall(haystack))
    missing = sorted(named - present)
    if missing:
        print("LOOP.md names instruments that no source or tool reads:")
        for t in missing:
            print(f"    {t}")
        print("  Either the switch was removed (update LOOP.md) or it never "
              "existed (do not plan an iteration around it).")
        return 1
    print(f"ai instruments: all {len(named)} OD_* switches named in LOOP.md exist")
    return 0


if __name__ == "__main__":
    sys.exit(main())
