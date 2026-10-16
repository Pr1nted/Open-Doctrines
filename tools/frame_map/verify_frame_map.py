#!/usr/bin/env python3
"""Check a --simulate log against the .expected.json written by
make_frame_map.py --probes N: every [FRAMEMAP] line must show the right frame
for its turn, the right number of BLK provinces and the right probe owners.

    python3 tools/frame_map/verify_frame_map.py run.log map.expected.json
"""
import json
import re
import sys

log = open(sys.argv[1]).read()
e = json.load(open(sys.argv[2]))
ok = bad = 0
pat = r"frame (\d+) turn (\d+) flipped (\d+) blk (\d+)(?: probes (\S+))?"
for m in re.finditer(pat, log):
    f, t, b, pr = int(m[1]), int(m[2]), int(m[4]), m[5] or ""
    want_pr = "".join(e["probe_owners"][f]) if e["probes"] else ""
    good = b == e["blk"][f] and pr == want_pr and t % e["frames"] == f
    ok += good
    bad += not good
    if not good:
        print(f"MISMATCH frame {f} turn {t}: blk {b} want {e['blk'][f]}  probes {pr} want {want_pr}")
errs = [l for l in log.splitlines() if "[SCRIPT]" in l and "rror" in l]
print(f"{ok} frame(s) match, {bad} mismatch(es), {len(errs)} script error line(s)")
sys.exit(1 if bad or errs or not ok else 0)
