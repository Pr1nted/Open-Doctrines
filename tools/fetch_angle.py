#!/usr/bin/env python3
"""Fetch the pinned ANGLE bundle for an OD_ANGLE build.

    python3 tools/fetch_angle.py <dest dir> [--platform macos-universal] [--cache DIR]

Writes libEGL, libGLESv2, SwiftShader and licenses/ into <dest>, flat, which is
what CMakeLists.txt's OD_ANGLE_DIR expects. The bundles are built by
.github/workflows/angle-prebuilts.yml and pinned in tools/data/angle.json.

Every archive is checked against its pinned SHA-256 before a byte of it is
unpacked, and a member whose path would land outside <dest> is refused. A
platform with no published bundle yet is an error that says so, rather than an
empty folder the game then fails to find a library in.
"""
import argparse
import hashlib
import json
import os
import platform
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIN = os.path.join(ROOT, "tools", "data", "angle.json")


def host_platform():
    if sys.platform == "darwin":
        return "macos-universal"
    m = platform.machine().lower()
    if sys.platform.startswith("win"):
        return "windows-x86_64" if m in ("amd64", "x86_64") else f"windows-{m}"
    if sys.platform.startswith("linux"):
        return "linux-x86_64" if m in ("amd64", "x86_64") else f"linux-{m}"
    return f"{sys.platform}-{m}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dest")
    ap.add_argument("--platform", default=host_platform())
    ap.add_argument("--cache", default=None, help="keep the downloaded archive here")
    a = ap.parse_args()

    bundle = json.load(open(PIN))["bundles"].get(a.platform)
    if not bundle or not bundle.get("url"):
        print(f"fetch_angle: no ANGLE bundle is published for {a.platform} yet. Run the "
              f"'ANGLE prebuilts' workflow with publish ticked and pin its output in "
              f"tools/data/angle.json, or point OD_ANGLE_DIR at a folder of your own.",
              file=sys.stderr)
        return 3

    cache = a.cache or os.path.join(a.dest, ".cache")
    os.makedirs(cache, exist_ok=True)
    archive = os.path.join(cache, os.path.basename(bundle["url"]))
    if not os.path.exists(archive):
        tmp = archive + ".part"
        with urllib.request.urlopen(bundle["url"]) as r, open(tmp, "wb") as f:
            while chunk := r.read(1 << 20):
                f.write(chunk)
        os.replace(tmp, archive)

    digest = hashlib.sha256(open(archive, "rb").read()).hexdigest()
    if digest != bundle["sha256"]:
        os.remove(archive)
        print(f"fetch_angle: {os.path.basename(archive)} has sha256 {digest}, the pin says "
              f"{bundle['sha256']}. Refusing to unpack it.", file=sys.stderr)
        return 1

    dest = os.path.realpath(a.dest)
    with tarfile.open(archive) as t:
        for m in t.getmembers():
            target = os.path.realpath(os.path.join(dest, m.name))
            if not (target == dest or target.startswith(dest + os.sep)) or m.issym() or m.islnk():
                print(f"fetch_angle: refusing archive member {m.name}", file=sys.stderr)
                return 1
        t.extractall(dest)
    print(f"fetch_angle: ANGLE for {a.platform} in {a.dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
