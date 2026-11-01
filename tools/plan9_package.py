#!/usr/bin/env python3
"""The Plan 9 edition's source tree, ready for 9front's mk, and its release zip.

    tools/plan9_package.py --data plan9                  generate the world C into plan9/
    tools/plan9_package.py --tree build/plan9/src         a tree to copy into 9front
    tools/plan9_package.py --tree T --out dist/OpenDoctrines-plan9.zip [--bin DIR]
    tools/plan9_package.py --check dist/OpenDoctrines-plan9.zip

9front has no Python, so the two generated files (od_world_data.c and
od_map_data.c, from templeos/world.odw by tools/freertos_world.py --map) are
made here and travel with the source. The tree keeps the repository's layout,
so plan9/mkfile's ../freertos/core and ../templeos paths hold in both:

    OpenDoctrines-plan9/
      plan9/            mkfile, the console and the map, generated world C
      freertos/core/    the rules and the shell, shared with the FreeRTOS edition
      templeos/Sync.HH  the desktop game's combat constants
      play.txt          the scripted game CI plays (comments stripped)
      amd64/            6.opendoctrines and 6.odmap, when --bin has them
      README.txt, VERSION
"""
import argparse
import os
import shutil
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import odver

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOP = "OpenDoctrines-plan9/"
CORE = ["od_world.c", "od_world.h", "od_rules.c", "od_rules.h", "od_shell.c", "od_shell.h"]
PLAN9 = ["mkfile", "opendoctrines.c", "odmap.c", "od_map.h", "od9fmt.c", "od9fmt.h",
         "core_world.c", "core_rules.c", "core_shell.c", "README.md"]
BINS = ["6.opendoctrines", "6.odmap"]

README = """Open Doctrines on Plan 9 {version}

The FreeRTOS edition's rules and world -- 1,632 provinces, 183 countries --
built with 9front's own compilers: a console program, and a libdraw map.

    cd plan9
    mk install          # /amd64/bin/games/opendoctrines and odmap
    games/opendoctrines # in any window, or over a serial console
    games/odmap         # in a rio window

amd64/ holds the binaries CI built inside 9front, when it has them. The
source is the repository's: plan9/README.md says what this edition is.
"""


def data(dest):
    os.makedirs(dest, exist_ok=True)
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "freertos_world.py"),
                           os.path.join(ROOT, "templeos", "world.odw"),
                           os.path.join(dest, "od_world_data.c"),
                           "--map", os.path.join(dest, "od_map_data.c")])


def tree(dest):
    if os.path.exists(dest):
        shutil.rmtree(dest)
    for sub in ("plan9/include", "freertos/core", "templeos"):
        os.makedirs(os.path.join(dest, sub))
    for f in PLAN9:
        shutil.copy(os.path.join(ROOT, "plan9", f), os.path.join(dest, "plan9", f))
    for f in sorted(os.listdir(os.path.join(ROOT, "plan9", "include"))):
        shutil.copy(os.path.join(ROOT, "plan9", "include", f), os.path.join(dest, "plan9", "include", f))
    for f in CORE:
        shutil.copy(os.path.join(ROOT, "freertos", "core", f), os.path.join(dest, "freertos", "core", f))
    shutil.copy(os.path.join(ROOT, "templeos", "Sync.HH"), os.path.join(dest, "templeos", "Sync.HH"))
    data(os.path.join(dest, "plan9"))
    with open(os.path.join(ROOT, "tests", "freertos", "play.txt")) as f:
        play = [l for l in f if l.strip() and not l.startswith("#")]
    with open(os.path.join(dest, "play.txt"), "w") as f:
        f.writelines(play)
    version = str(odver.read())
    with open(os.path.join(dest, "README.txt"), "w") as f:
        f.write(README.format(version=version))
    with open(os.path.join(dest, "VERSION"), "w") as f:
        f.write(version + "\n")


def pack(src, out, bindir):
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _dirs, files in sorted(os.walk(src)):
            for f in sorted(files):
                p = os.path.join(base, f)
                z.write(p, TOP + os.path.relpath(p, src).replace(os.sep, "/"))
        if bindir:
            for b in BINS:
                p = os.path.join(bindir, b)
                if os.path.exists(p):
                    z.write(p, TOP + "amd64/" + b)
    print(f"wrote {out}")


def check(path):
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
    need = [TOP + "plan9/" + f for f in PLAN9 + ["od_world_data.c", "od_map_data.c"]]
    need += [TOP + "freertos/core/" + f for f in CORE]
    need += [TOP + "templeos/Sync.HH", TOP + "play.txt", TOP + "README.txt", TOP + "VERSION"]
    missing = [n for n in need if n not in names]
    for m in missing:
        print(f"missing: {m}")
    bins = [b for b in BINS if TOP + "amd64/" + b in names]
    print(f"{path}: {len(names)} files, binaries: {', '.join(bins) or 'none'}")
    return 1 if missing else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", help="write the generated world C into this directory")
    ap.add_argument("--tree", help="assemble the source tree here")
    ap.add_argument("--out", help="zip the tree to this file")
    ap.add_argument("--bin", help="a directory holding 6.opendoctrines and 6.odmap")
    ap.add_argument("--check", help="check a zip")
    a = ap.parse_args()
    if a.check:
        return check(a.check)
    if a.data:
        data(a.data)
    if a.tree:
        tree(a.tree)
        if a.out:
            pack(a.tree, a.out, a.bin)
    elif a.out:
        ap.error("--out needs --tree")
    if not (a.data or a.tree):
        ap.error("nothing to do: --data, --tree or --check")
    return 0


if __name__ == "__main__":
    sys.exit(main())
