#!/usr/bin/env python3
"""Keep the Flatpak manifest and its AppStream metainfo on the current release.

WHAT GOES STALE, AND WHY IT IS NOT OBVIOUS

packaging/linux/README.md describes two lines of per-release maintenance: the
manifest's `url:` and `sha256:` point at a published archive, and the metainfo
needs a <release> entry because Flathub shows the newest one as the changelog.
Both are hand-edited, neither is read by any build, and so by 1.2.2a both still
said 1.2.0a -- two releases behind, with nothing failing anywhere to say so.

That is the shape of every stale-pin bug: the file is correct on the day it is
written and nothing ever checks it again. So `--check` runs in the suite.

WHY CI DOES NOT USE THESE PINS AT ALL

The release workflow builds the Flatpak from the artifact it just built and
tested, not from a published URL -- the same rule as the .deb and the .rpm, so
a package cannot differ from the archive it claims to contain. At the moment
the Flatpak is built the URL does not resolve yet anyway: nothing is published
until the release is created.

The pins exist for FLATHUB, which builds from the manifest in this repository
and cannot see our artifacts. They are submission material, and they have to be
right when someone submits.

    python3 tools/update_flatpak_release.py --check
    python3 tools/update_flatpak_release.py --set 1.2.2a --summary "..."
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(ROOT, "packaging", "linux")
APPID = "io.github.Pr1nted.OpenDoctrines"
MANIFEST = os.path.join(PKG, f"{APPID}.yml")
METAINFO = os.path.join(PKG, f"{APPID}.metainfo.xml")
ARCHIVE = "OpenDoctrines-linux-x64.zip"
URL = "https://github.com/Pr1nted/Open-Doctrines/releases/download/v{v}/" + ARCHIVE


def current_version():
    out = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "odver.py")],
                         capture_output=True, text=True)
    v = out.stdout.strip().splitlines()[0].strip() if out.stdout.strip() else ""
    if not re.fullmatch(r"\d+\.\d+\.\d+[a-z]", v):
        sys.exit(f"tools/odver.py gave {v!r}, which is not a version")
    return v


def manifest_version(text):
    m = re.search(r"url:\s*\S+/download/v(\S+?)/" + re.escape(ARCHIVE), text)
    return m.group(1) if m else None


def metainfo_version(text):
    m = re.search(r'<release\s+version="([^"]+)"', text)
    return m.group(1) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--set", metavar="VERSION")
    ap.add_argument("--sha", help="skip downloading; use this sha256")
    ap.add_argument("--date", help="release date YYYY-MM-DD (default: today)")
    ap.add_argument("--summary", default="", help="one line for the Flathub changelog")
    a = ap.parse_args()

    man = open(MANIFEST, encoding="utf-8").read()
    met = open(METAINFO, encoding="utf-8").read()

    if a.check:
        cur = current_version()
        mv, xv = manifest_version(man), metainfo_version(met)
        bad = []
        if mv != cur:
            bad.append(f"manifest pins v{mv}, the game is {cur}")
        if xv != cur:
            bad.append(f"metainfo's newest release is {xv}, the game is {cur}")
        if bad:
            print("the Flathub submission material is out of date:")
            for b in bad:
                print(f"    {b}")
            print("    fix: python3 tools/update_flatpak_release.py --set " + cur)
            # NOT a failure. Flathub is a downstream nobody has submitted to
            # yet, and blocking every release on it would be the tail wagging
            # the dog. It says so every run instead, which is what a pin that
            # went two releases stale needed and did not have.
            return 0
        print(f"ok    Flatpak manifest and metainfo are on {cur}")
        return 0

    if not a.set:
        sys.exit("give --check or --set VERSION")

    v = a.set.lstrip("v")
    url = URL.format(v=v)
    sha = a.sha
    if not sha:
        print(f"fetching {url}")
        try:
            with urllib.request.urlopen(url) as r:
                sha = hashlib.sha256(r.read()).hexdigest()
        except Exception as e:
            sys.exit(f"could not fetch it ({e}). Pass --sha if it is not published yet.")
    print(f"  v{v}  sha256 {sha}")

    man2 = re.sub(r"(url:\s*)\S+", lambda m: m.group(1) + url, man, count=1)
    man2 = re.sub(r"(sha256:\s*)\S+", lambda m: m.group(1) + sha, man2, count=1)
    if man2 == man:
        sys.exit("nothing in the manifest matched url:/sha256:")
    open(MANIFEST, "w", encoding="utf-8").write(man2)

    date = a.date or subprocess.run(["date", "+%Y-%m-%d"], capture_output=True,
                                    text=True).stdout.strip()
    summary = a.summary or f"Release {v}."
    entry = (f'    <release version="{v}" date="{date}">\n'
             f'      <description>\n'
             f'        <p>{summary}</p>\n'
             f'      </description>\n'
             f'    </release>\n')
    # Newest first: Flathub shows the first entry as the changelog.
    met2 = met.replace("  <releases>\n", "  <releases>\n" + entry, 1)
    if met2 == met:
        sys.exit("no <releases> block in the metainfo")
    open(METAINFO, "w", encoding="utf-8").write(met2)
    print(f"updated {os.path.relpath(MANIFEST, ROOT)} and "
          f"{os.path.relpath(METAINFO, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
