#!/usr/bin/env python3
"""Which CI tiers a change needs. Every workflow's plan job asks this one script.

    tools/ci_plan.py                      reads GITHUB_EVENT_NAME, GITHUB_EVENT_PATH
    tools/ci_plan.py --files a b c        decide for these changed paths (no git)
    tools/ci_plan.py --explain            print why each tier is on or off

Writes `name=true|false` lines to $GITHUB_OUTPUT (or stdout), one per tier:

    core     the four-platform suite (linux-x64, windows-x64, macos-arm64), the
             servers, web, android arm64, sanitizers, translation layer. Always.
    wide     everything the core suite already proves on one more CPU: the
             emulated Linux arches, windows-x86/arm64, macos-x64, the other
             three Android ABIs and the APK
    bsd      FreeBSD, OpenBSD and NetBSD in their VMs
    otheros  the other operating systems (OpenIndiana, Haiku, ... ) in theirs
    full     all of the above; set for the nightly run, a release, a manual
             run, and a pull request labelled ci:full

WHY TIERS, AND WHY THIS SCRIPT DECIDES THEM

The suite used to be one shape for every event: ~35 jobs on every push to a
pull request and again on main after it merged. With a fixed number of runner
slots that is a queue, not a pipeline -- a stack of port PRs spent two days
waiting for machines, mostly to re-prove on a ninth CPU what eight had already
proved about a change that did not touch anything CPU- or OS-specific.

So a pull request runs the core tier, plus the platforms its files can
actually break; the nightly run on main runs everything and is where a slow
platform's regression surfaces when nothing in the diff pointed at it. The
rules are paths, kept here and not in four workflows, so "what does a change
to X run" has one answer.
"""
import argparse
import fnmatch
import json
import os
import subprocess
import sys

# A path that can change behaviour on every platform at once: the build, the
# CI itself, the vendored libraries and the prebuilt archives. Any of these
# turns everything on.
EVERYTHING = [
    "CMakeLists.txt", "cmake/*", "cmake/**",
    ".github/workflows/*", ".github/actions/**",
    "third_party/**",
    "tools/ci_plan.py", "tools/qualify.sh", "tools/ci_*",
]

# What can break on a different CPU while passing on the core three: code that
# reads bytes, sizes or sockets, and the platform layer.
WIDE = [
    "src/platform/**", "src/net/**", "src/util/**", "src/save/**",
    "src/Save*", "src/*Serial*", "src/odseal*", "src/mods/**",
    "tests/campaign_*", "tests/run_all.sh",
    "tools/package*.py", "packaging/**",
    "android/**", "tools/package_android.sh",
]

BSD = [
    "tools/*bsd*", "src/platform/bsd/**", "src/platform/*bsd*",
    ".github/workflows/bsd-game.yml",
]

OTHEROS = [
    "src/platform/**",
    ".github/workflows/other-os-game.yml",
    "tools/*illumos*", "tools/*haiku*", "tools/*serenity*", "tools/*vita*",
    "tools/*dos*", "tools/*djgpp*", "tools/*wii*", "tools/*3ds*", "tools/*riscos*",
    "src/rlsw*", "src/*Rlsw*", "src/*Small*",
]

FULL_EVENTS = {"schedule", "workflow_dispatch", "workflow_call", "release"}


def match(path, patterns):
    return any(fnmatch.fnmatchcase(path, p) for p in patterns)


def event():
    path = os.environ.get("GITHUB_EVENT_PATH")
    if not path or not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def changed_files(ev):
    """Files the pull request changes, against the base it will merge into."""
    pr = ev.get("pull_request") or {}
    base = (pr.get("base") or {}).get("sha")
    head = (pr.get("head") or {}).get("sha")
    if not (base and head):
        return None
    try:
        out = subprocess.run(["git", "diff", "--name-only", f"{base}...{head}"],
                             check=True, capture_output=True, text=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None          # a shallow checkout without the base: run it all
    return [l for l in out.splitlines() if l]


def decide(name, files, labels):
    why = {}
    if name in FULL_EVENTS or (name == "push" and os.environ.get("GITHUB_REF", "").startswith("refs/tags/")):
        why["full"] = f"{name} runs everything"
    elif "ci:full" in labels:
        why["full"] = "labelled ci:full"
    elif files is None:
        why["full"] = "the changed files could not be listed"
    else:
        hit = [f for f in files if match(f, EVERYTHING)]
        if hit:
            why["full"] = f"{hit[0]} can change every platform"
    full = "full" in why

    tiers = {"core": True, "full": full}
    for tier, pats in (("wide", WIDE), ("bsd", BSD), ("otheros", OTHEROS)):
        if full:
            tiers[tier] = True
            continue
        hit = [f for f in (files or []) if match(f, pats)]
        tiers[tier] = bool(hit)
        why[tier] = (f"{hit[0]} is in its paths" if hit else "nothing in its paths changed")
    return tiers, why


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--files", nargs="*")
    ap.add_argument("--event", default=os.environ.get("GITHUB_EVENT_NAME", ""))
    ap.add_argument("--labels", nargs="*", default=None)
    ap.add_argument("--explain", action="store_true")
    a = ap.parse_args()

    ev = event()
    labels = a.labels if a.labels is not None else [
        l.get("name", "") for l in (ev.get("pull_request") or {}).get("labels", [])]
    files = a.files if a.files is not None else (
        changed_files(ev) if a.event == "pull_request" else [])
    tiers, why = decide(a.event, files, labels)

    out = os.environ.get("GITHUB_OUTPUT")
    lines = [f"{k}={'true' if v else 'false'}" for k, v in tiers.items()]
    if out:
        with open(out, "a", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    if a.explain or out:
        for k, v in why.items():
            print(f"  {k}: {v}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
