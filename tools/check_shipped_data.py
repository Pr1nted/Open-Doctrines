#!/usr/bin/env python3
"""Check that the two lists of shipped data agree, and that data/ is classified.

    tools/check_shipped_data.py

What a player receives is decided in two places: OD_SHIPPED_DATA in
CMakeLists.txt (the web preload, Android and the installers) and DATA_ALLOWLIST
in tools/release.py (the zips, via tools/package.py). They drifted: 1.2.2a
shipped without district_laws.json and parties.json everywhere, and without
comms/ in the zips, while a comment said the two lists must agree. This fails
when they do not, and when a tracked file in data/ is in neither the shipped
list nor tools/release.py's KNOWN_USER_DATA.
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import release  # noqa: E402


def cmake_list():
    # Comments first: the block's own comments contain parentheses.
    lines = open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8").read().splitlines()
    code = "\n".join(line.split("#", 1)[0] for line in lines)
    m = re.search(r"set\(OD_SHIPPED_DATA\b([^)]*)\)", code)
    if not m:
        sys.exit("CMakeLists.txt: no set(OD_SHIPPED_DATA ...)")
    return set(m.group(1).split())


def odstate_list():
    """The names src/OdState.cpp treats as the build's rather than the player's."""
    src = open(os.path.join(ROOT, "src", "OdState.cpp"), encoding="utf-8").read()
    m = re.search(r"const char\* kShipped\[\] = \{(.*?)\};", src, re.S)
    if not m:
        sys.exit("src/OdState.cpp: no kShipped[]")
    body = "\n".join(line.split("//", 1)[0] for line in m.group(1).splitlines())
    return set(re.findall(r'"([^"]+)"', body))


def main():
    fails = 0
    cmake = cmake_list()
    py = set(release.DATA_ALLOWLIST)
    for name in sorted(cmake - py):
        print(f"FAIL  {name}: shipped by CMake (web, Android, installers), not by release.py (zips)")
        fails += 1
    for name in sorted(py - cmake):
        print(f"FAIL  {name}: shipped by release.py (zips), not by CMake (web, Android, installers)")
        fails += 1

    # THE THIRD LIST, and the one whose drift is not merely cosmetic.
    #
    # src/OdState.cpp decides what a .odstate holds -- everything under data/
    # EXCEPT the shipped content. In the browser that archive is rebuilt and
    # pushed to IndexedDB whenever the player's state changes, on the frame
    # thread, so a shipped directory missing from kShipped[] is not a fatter
    # file: it is megabytes of the build's own content re-compressed while the
    # game is not drawing. lang/, dialog/ and comms/ were missing, which is
    # 7 MB, and the freeze that followed was reported by a player.
    #
    # One direction only. kShipped[] legitimately holds names CMake does not
    # (Icon, MANAGED, VERSION, tools), because they are not content either.
    odstate = odstate_list()
    for name in sorted(cmake):
        top = name.split("/", 1)[0]
        if top not in odstate:
            print(f"FAIL  {top}: shipped by CMake, but src/OdState.cpp archives it "
                  f"as the player's -- add it to kShipped[]")
            fails += 1

    tracked = subprocess.run(["git", "ls-files", "data"], cwd=ROOT, capture_output=True,
                             text=True, check=True).stdout.split()
    top = sorted({p.split("/")[1] for p in tracked if p.count("/") >= 1})
    shipped_top = {n.split("/", 1)[0] for n in py}
    for name in top:
        if name not in shipped_top and name not in release.KNOWN_USER_DATA:
            print(f"FAIL  data/{name} is tracked but neither shipped nor in KNOWN_USER_DATA")
            fails += 1

    if fails:
        print(f"\n{fails} problem(s). Ship it in BOTH lists, or add it to KNOWN_USER_DATA.")
        return 1
    print(f"ok  {len(py)} shipped entries agree across CMake, release.py and "
          f"OdState.cpp; {len(top)} tracked data/ entries are all classified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
