#!/usr/bin/env python3
"""Release util. Two pipelines, one command.

    tools/release.py game [--new-update] [--switch-state a|b|r|snapshot]
                          [--notes FILE] [--dry-run] [--no-push]

    tools/release.py sdk  [--major] [--notes FILE] [--dry-run] [--no-push]

WHY TWO PIPELINES

The game and the SDK live in one repository but do not ship together. A player
on an old build still needs the SDK docs for the ABI that build implements, and
a modder should not be told to update the game because the SDK moved. So they
version, tag and release independently:

    game tags   v1.0.3a          -> GitHub release + itch.io
    sdk tags    gearbox-v1.1     -> GitHub release only

WHAT THIS DOES NOT DO

It does not build for Windows or Linux, and it does not run the confirmation
gate. Those live in .github/workflows/, because they need machines this script
is not running on. What this does is get the repository into the exact state
that triggers them: version bumped, notes written, committed, tagged, pushed.

SECRETS

Nothing here reads, writes or logs a credential. The itch.io push happens in CI
from an encrypted secret. If that secret is absent the workflow warns and
carries on -- a missing itch key is not a reason to fail a release.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import odver  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The commit identity every release is authored as, per project convention.
AUTHOR_NAME = "Pr1nted"
AUTHOR_EMAIL = ""          # deliberately empty

# What a shipped copy of data/ is allowed to contain.
#
# An ALLOWLIST, not a denylist: the failure mode of a denylist is that a new
# kind of user file -- a new export folder, a new cache -- ships to players
# because nobody remembered to exclude it. Here anything unlisted is simply
# absent, and adding shipped content is a deliberate edit.
#
# The cost of that shape is the opposite mistake, and this list has made it:
# content that is missing is also simply absent, and the game does not complain.
# Nothing here belongs to a map -- policies.json, starting_policies.json,
# country_compass.json and the rest are entries INSIDE the .odmap archives
# under STDmaps/ (Game::initPolicies reads them from m_odmJsonData). A
# top-level "policies.json" sat in this list for a long time, matched no file,
# and printed one line about it per package.
DATA_ALLOWLIST = [
    "STDmaps",        # the shipped maps
    "audio",          # music and sfx. Its absence did not fail anything: the
                      # game starts, reports "Audio ready (0 sounds, 0 tracks)"
                      # and plays in silence, so every release before this line
                      # existed shipped a silent game.
    "flags",
    "fonts",
    "icons",
    "symbols",
    "licenses",
    "ai",             # the trained model is game content
    "tips.json",
    "credits.txt",
]

# Present in a working copy, never in a release. Listed only so the packaging
# step can say what it left out, which is how you notice a mistake.
KNOWN_USER_DATA = [
    "config.json",    # the developer's own settings
    "saves",          # the developer's own saves
    "custom_maps",    # worlds made or imported locally
    "exports",
    "projects",
    "timelapses",
    "mods",           # locally installed mods, and their storage
    "mods.json",      # which mods were enabled, and their grants
    "tools",          # cloudflared, which the game downloads for itself when a
                      # host asks for a tunnel (src/net/TunnelInstall.cpp). It
                      # is fetched per-platform and verified on arrival, so
                      # shipping one machine's copy would add ~37 MB to every
                      # download and be the wrong binary for most of them.
    ".DS_Store",      # macOS leaves these everywhere
    "Icon\r",         # macOS custom-folder-icon marker; the name really does
    "Icon",           # end in a carriage return, so both spellings are listed
]


# ------------------------------------------------------------------ shell ---

def run(cmd, check=True, capture=False, cwd=ROOT):
    if capture:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
        if check and r.returncode != 0:
            fail(f"{' '.join(cmd)}\n{r.stdout}{r.stderr}")
        return r.stdout.strip()
    r = subprocess.run(cmd, cwd=cwd)
    if check and r.returncode != 0:
        fail(f"command failed: {' '.join(cmd)}")
    return r.returncode


def say(msg):  print(f"  {msg}")
def step(msg): print(f"\n=== {msg} ===")
def warn(msg): print(f"  WARNING: {msg}")


def fail(msg):
    print(f"\nFAILED: {msg}", file=sys.stderr)
    sys.exit(1)


# ------------------------------------------------------------ shared bits ---

def require_clean_tree(dry_run):
    dirty = run(["git", "status", "--porcelain"], capture=True)
    if not dirty:
        return
    if dry_run:
        warn("working tree is dirty; a real run would refuse")
        return
    fail("working tree is not clean. Commit or stash first -- a release should\n"
         "        be reproducible from the tag, and it cannot be if it was cut\n"
         "        from uncommitted work.\n\n" + dirty)


def read_notes(path, version, kind):
    """Patch notes. Required for a real release: a version with no notes is one
    nobody can tell apart from the last one."""
    if path == "-":
        print(f"Enter patch notes for {kind} {version}. End with Ctrl-D.")
        return sys.stdin.read().strip()
    if path:
        if not os.path.exists(path):
            fail(f"no such notes file: {path}")
        with open(path) as f:
            return f.read().strip()
    return ""


def write_changelog(kind, version, notes):
    """Prepends to CHANGELOG.md. Newest first, so the top of the file is
    always the current release."""
    path = os.path.join(ROOT, "CHANGELOG.md")
    header = f"## {kind} {version}\n\n{notes}\n"
    existing = ""
    if os.path.exists(path):
        with open(path) as f:
            existing = f.read()
        if existing.startswith("# Changelog\n"):
            existing = existing[len("# Changelog\n"):].lstrip("\n")
    with open(path, "w") as f:
        f.write("# Changelog\n\n" + header + "\n" + existing)
    say(f"changelog updated: {path}")


def commit_and_tag(paths, message, tag, dry_run, no_push):
    if dry_run:
        say(f"[dry-run] would commit {paths} as {AUTHOR_NAME}")
        say(f"[dry-run] would tag {tag}")
        say("[dry-run] would push" if not no_push else "[dry-run] push disabled")
        return

    run(["git", "add"] + paths)
    # An empty email is intentional and has to be spelled this way; git will not
    # take --author="Pr1nted" without the angle brackets.
    run(["git", "commit",
         "--author", f"{AUTHOR_NAME} <{AUTHOR_EMAIL}>",
         "-m", message])
    run(["git", "tag", "-a", tag, "-m", message])
    say(f"committed and tagged {tag}")

    if no_push:
        warn("--no-push: nothing was pushed, so no workflow will run")
        return
    run(["git", "push"])
    run(["git", "push", "origin", tag])
    say(f"pushed {tag}; the release workflow takes over from here")


# ------------------------------------------------------------------- game ---

def cmd_game(args):
    step("game release")
    current = odver.read()
    nxt = current.bump(new_update=args.new_update, state=args.switch_state)
    say(f"version {current} -> {nxt}")
    if args.switch_state:
        say(f"state {odver.STATE_NAMES[current.state]} -> "
            f"{odver.STATE_NAMES[nxt.state]}")

    require_clean_tree(args.dry_run)

    notes = read_notes(args.notes, nxt, "game")
    if not notes and not args.dry_run:
        fail("patch notes are required for a game release (--notes FILE, or "
             "--notes - to type them)")

    # The build gate. A release that does not compile is not a release, and
    # finding that out in CI after the tag is pushed is too late.
    step("build")
    if args.skip_build:
        warn("--skip-build: the build gate was skipped")
    else:
        run(["cmake", "-S", ".", "-B", "build"], capture=True)
        rc = run(["cmake", "--build", "build", "--target", "OpenDoctrines",
                  "-j", str(os.cpu_count() or 4)], check=False)
        if rc != 0:
            fail("the game does not build. Nothing was bumped, committed or "
                 "tagged.")
        say("game builds")

    step("tests")
    if args.skip_tests:
        warn("--skip-tests: the test gate was skipped")
    else:
        rc = run([os.path.join(ROOT, "tests", "run_all.sh")], check=False)
        if rc != 0:
            fail("tests failed. Nothing was bumped, committed or tagged.")
        say("tests pass")

    step("bump")
    if args.dry_run:
        say(f"[dry-run] would write VERSION = {nxt}")
    else:
        odver.write(nxt)
        write_changelog("game", nxt, notes)
        say(f"VERSION = {nxt}")

    step("commit")
    commit_and_tag(["VERSION", "CHANGELOG.md"],
                   f"Release {nxt}\n\n{notes}", f"v{nxt}",
                   args.dry_run, args.no_push)

    step("next")
    say("The GitHub Actions release workflow now:")
    say("  1. builds macOS, Windows and Linux")
    say("  2. uploads artifacts and WAITS for you to play each one")
    say("  3. publishes only after you approve, then pushes to itch.io")
    if not os.environ.get("BUTLER_API_KEY"):
        warn("BUTLER_API_KEY is not set locally. That is fine -- the itch.io "
             "push uses the repository secret, and is skipped with a warning "
             "if that is absent too.")


# -------------------------------------------------------------------- sdk ---

def sdk_version():
    """Gearbox MAJOR.MINOR, read from the ABI rather than a second file."""
    import json
    with open(os.path.join(ROOT, "sdk", "abi.json")) as f:
        abi = json.load(f)
    c = abi["constants"]
    return int(c["GEARBOX_MAJOR"]), int(c["GEARBOX_MINOR"])


def set_sdk_version(major, minor, dry_run):
    """The Gearbox version appears in three places that must agree: abi.json's
    constants, the ABI's own "gearbox" field, and the host's compatibility
    check in ModPackage.cpp. ModAbiTest pins the first two together; the third
    is what decides whether a mod loads, so it is updated here too."""
    import json
    changed = []

    ap = os.path.join(ROOT, "sdk", "abi.json")
    with open(ap) as f:
        abi = json.load(f)
    abi["constants"]["GEARBOX_MAJOR"] = major
    abi["constants"]["GEARBOX_MINOR"] = minor
    abi["gearbox"] = f"{major}.{minor}"
    if not dry_run:
        with open(ap, "w") as f:
            json.dump(abi, f, indent=2)
            f.write("\n")
    changed.append("sdk/abi.json")

    mp = os.path.join(ROOT, "src", "mods", "ModPackage.cpp")
    with open(mp) as f:
        src = f.read()
    src2 = re.sub(r"(kHostGearboxMajor\s*=\s*)\d+", rf"\g<1>{major}", src)
    src2 = re.sub(r"(kHostGearboxMinor\s*=\s*)\d+", rf"\g<1>{minor}", src2)
    if src2 != src:
        if not dry_run:
            with open(mp, "w") as f:
                f.write(src2)
        changed.append("src/mods/ModPackage.cpp")

    return changed


def cmd_sdk(args):
    step("sdk release")
    major, minor = sdk_version()
    if args.major:
        nmajor, nminor = major + 1, 0
    else:
        nmajor, nminor = major, minor + 1
    say(f"Gearbox {major}.{minor} -> {nmajor}.{nminor}")
    if args.major:
        say("major bump: every existing mod targeting the old major will be "
            "refused at load, by design")

    require_clean_tree(args.dry_run)

    notes = read_notes(args.notes, f"{nmajor}.{nminor}", "sdk")
    if not notes and not args.dry_run:
        fail("patch notes are required for an SDK release (--notes FILE, or "
             "--notes - to type them)")

    # Every SDK, every test, before anything is bumped. This is the whole point
    # of a separate pipeline: the SDK ships when the SDKs are green, not when
    # the game happens to be.
    step("sdk tests")
    if args.skip_tests:
        warn("--skip-tests: the test gate was skipped")
    else:
        rc = run([os.path.join(ROOT, "tests", "run_all.sh")], check=False)
        if rc != 0:
            fail("SDK tests failed. Nothing was bumped, committed or tagged.")
        say("all SDKs pass")

    step("bump")
    changed = set_sdk_version(nmajor, nminor, args.dry_run)
    for c in changed:
        say(("[dry-run] would update " if args.dry_run else "updated ") + c)

    if not args.dry_run:
        # Regenerate so the bindings and docs carry the new version.
        run(["python3", os.path.join(ROOT, "tools", "gen_bindings.py")], capture=True)
        run(["python3", os.path.join(ROOT, "tools", "gen_abi_docs.py")], capture=True)
        say("regenerated bindings and ABI docs")
        write_changelog("sdk", f"{nmajor}.{nminor}", notes)
        changed += ["CHANGELOG.md", "docs/gearbox-abi.md", "sdk/"]

    step("commit")
    commit_and_tag(list(dict.fromkeys(changed)),
                   f"Gearbox SDK {nmajor}.{nminor}\n\n{notes}",
                   f"gearbox-v{nmajor}.{nminor}",
                   args.dry_run, args.no_push)

    step("next")
    say("The SDK workflow publishes a GitHub release with the SDK sources,")
    say("the generated bindings and the ABI reference. GitHub only -- the SDK")
    say("is not an itch.io artifact.")


# ------------------------------------------------------------------- main ---

def main():
    ap = argparse.ArgumentParser(
        description="Release the game or the SDK.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    sub = ap.add_subparsers(dest="which", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--notes", metavar="FILE",
                        help="patch notes file, or - to type them")
    common.add_argument("--dry-run", action="store_true",
                        help="say what would happen and change nothing")
    common.add_argument("--no-push", action="store_true",
                        help="commit and tag locally, do not push")
    common.add_argument("--skip-tests", action="store_true",
                        help="skip the test gate (do not use for a real release)")

    g = sub.add_parser("game", parents=[common], help="release the game")
    g.add_argument("--new-update", action="store_true",
                   help="bump MINOR (1.0.2a -> 1.1.2a) instead of PATCH")
    g.add_argument("-switch-state", "--switch-state", dest="switch_state",
                   choices=sorted(odver.STATES.keys()),
                   help="change the state letter: alpha/a, beta/b, release/r, snapshot/s")
    g.add_argument("--skip-build", action="store_true",
                   help="skip the build gate (do not use for a real release)")
    g.set_defaults(fn=cmd_game)

    s = sub.add_parser("sdk", parents=[common], help="release the Gearbox SDK")
    s.add_argument("-major", "--major", dest="major", action="store_true",
                   help="bump Gearbox MAJOR and reset MINOR to 0")
    s.set_defaults(fn=cmd_sdk)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
