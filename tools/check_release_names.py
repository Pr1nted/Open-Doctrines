#!/usr/bin/env python3
"""Three lists of platform names that must agree, checked as one.

    python3 tools/check_release_names.py

1. GameUpdates::platformKey() (src/GameUpdates.cpp) -- the release asset the
   in-game updater downloads for the build it is running in.
2. The artifacts the release workflows build (release-game.yml, bsd-game.yml,
   other-os-game.yml)
   -- what actually gets attached to a release as OpenDoctrines-<x>.zip.
3. The download page's buttons (packaging/web/site/download.html).
4. The dedicated server's platforms (release-server.yml), which must match the
   game's desktop platforms -- the server is what people run on a Pi or an Arm
   VPS -- apart from the BSDs (FreeBSD, OpenBSD, NetBSD), the other-os
   systems (illumos, Haiku, SerenityOS), DOS and the Wii, which have no server
   build (DOS and the Wii have no network to serve on), and the Raspberry Pi OS
   game builds,
   whose server is the Linux arm64/armv7 one: a server draws nothing, so
   OpenGL ES changes nothing for it.
5. The PS Vita's .vpk, which no updater key names (a .vpk is installed by
   VitaShell, never unpacked over the game -- as the APK): the page's .vpk
   button must name what other-os-game.yml uploads.

WHY. They are maintained by hand in three files, and every way they can drift
is silent. An updater key with no artifact means "no download for this
platform" for every player on it; an artifact no key names is a build no
installed copy can ever update to; a page button for a name nobody builds is a
404 the page hides, so nobody notices. The updater was already wrong this way
before the 32-bit and Arm ports -- every Linux build asked for linux-x64 -- and
nothing caught it.

Exit 0 when the lists agree, 1 with the differences when they do not.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def updater_keys():
    src = read("src/GameUpdates.cpp")
    a = src.index("std::string GameUpdates::platformKey()")
    b = src.index("\n}\n", a)
    names = set(re.findall(r'return\s+"(OpenDoctrines-[A-Za-z0-9_-]+)"', src[a:b]))
    # The keys that deliberately match nothing: unknown architectures and the
    # APK, which is never replaced by unpacking a zip.
    return {n for n in names if not n.endswith(("-unknown", "-none")) and n != "OpenDoctrines-unknown"}


def built_artifacts():
    names = set(re.findall(r"artifact:\s*(OpenDoctrines-[A-Za-z0-9_-]+)",
                           read(".github/workflows/release-game.yml")))
    # bsd-game.yml builds its matrix in a plan step, so its names are the
    # entries' "name" fields, released as OpenDoctrines-<name> (the plan step
    # sets "artifact" that way).
    names |= {"OpenDoctrines-" + n for n in
              re.findall(r'"name":\s*"([a-z]+bsd-[A-Za-z0-9_-]+)"', read(".github/workflows/bsd-game.yml"))}
    # other-os-game.yml names its entries in a YAML matrix, not a JSON plan.
    names |= {"OpenDoctrines-" + n for n in
              re.findall(r"-\s*\{\s*name:\s*([a-z]+-[A-Za-z0-9_]+)\s*,", read(".github/workflows/other-os-game.yml"))}
    # The browser build is a page, not an install, and has no updater key.
    return {n for n in names if not n.endswith("-web")}


def edition_zips():
    # Editions with releases of their own (freertos-v* tags): the page offers
    # them, but no build's updater ever replaces itself with one.
    return set(re.findall(r"dist/(OpenDoctrines-[A-Za-z0-9_-]+)\.zip",
                          read(".github/workflows/freertos.yml")))


def server_platforms():
    names = re.findall(r"artifact:\s*OpenDoctrinesServer-([A-Za-z0-9_-]+)",
                       read(".github/workflows/release-server.yml"))
    return set(names)


def page_zips():
    html = read("packaging/web/site/download.html")
    return {m[: -len(".zip")] for m in re.findall(r'data-asset="(OpenDoctrines-[^"]+\.zip)"', html)}


def page_vpks():
    html = read("packaging/web/site/download.html")
    return set(re.findall(r'data-asset="(OpenDoctrines-[^"]+)\.vpk"', html))


def built_vpks():
    # The Vita job's ARTIFACT, uploaded as dist/<ARTIFACT>.vpk.
    wf = read(".github/workflows/other-os-game.yml")
    if ".vpk" not in wf:
        return set()
    return set(re.findall(r"ARTIFACT:\s*(OpenDoctrines-vita[A-Za-z0-9_-]*)", wf))


def main():
    keys, built, page = updater_keys(), built_artifacts(), page_zips()
    problems = []
    for n in sorted(page_vpks() - built_vpks()):
        problems.append(f"download page offers {n}.vpk, which no release workflow builds")
    for n in sorted(keys - built):
        problems.append(f"updater asks for {n}.zip, but no release workflow builds it")
    for n in sorted(built - keys):
        problems.append(f"{n}.zip is released, but no build's updater ever asks for it")
    for n in sorted(page - built - edition_zips()):
        problems.append(f"download page offers {n}.zip, which no release workflow builds")
    # The dedicated server ships wherever the game does -- it is what people
    # run on a Pi or an Arm VPS -- except the BSDs, which have no server build.
    game_desktop = {n.replace("OpenDoctrines-", "") for n in built
                    if not re.match(r"OpenDoctrines-(?:[a-z]+bsd|illumos|haiku|serenity|raspios|dos|wii)-", n)}
    server = server_platforms()
    for p in sorted(game_desktop - server):
        problems.append(f"the game ships for {p}, but release-server.yml has no server for it")
    for p in sorted(server - game_desktop):
        problems.append(f"release-server.yml builds a server for {p}, which the game does not ship")
    if problems:
        print("release names disagree:")
        for p in problems:
            print("  " + p)
        return 1
    print(f"release names agree: {len(keys)} game platforms "
          f"({', '.join(n.replace('OpenDoctrines-', '') for n in sorted(keys))}); "
          f"server on {len(server)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
