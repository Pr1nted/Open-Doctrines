#!/usr/bin/env python3
"""A front end for tools/release.py that shows what a release is about to do.

    tools/release_tui.py              # the wizard
    tools/release_tui.py --no-network # skip the gh queries

WHY

tools/release.py is correct and unhelpful: it takes the decisions as flags, acts
on them, and the two rules that have cost the most releases are not among the
flags, because they are not about this repository's files.

    Tag only after `Test` on main is green on all five platforms, and green at
    the commit being tagged. 1.2.2a took six cuts because each failure hid the
    next, and the local suite was green through all six.

    Release the SDK BEFORE the game when the ABI moved. `release.py sdk` is
    what bumps the Gearbox version, in the working tree, so a game tag cut
    first ships a host declaring the old ABI minor while implementing the new
    one. Game 1.0.6a and SDK 1.1 both had to be re-cut for this.

Neither rule can be checked by reading the tree alone: the first lives in GitHub
Actions and the second is a diff against a tag. This reads both, prints the
answers next to the versions, and refuses to run a release that contradicts
them -- while release.py keeps doing the work, so there is one implementation of
a release and this is a window onto it.

NOTHING HERE RELEASES ANYTHING ITSELF

Every action is `tools/release.py` with arguments, printed in full before it
runs and run only after a typed yes. The wizard holds no state between runs:
every number on the screen is read again each time it draws, so a release
interrupted anywhere can be resumed by running the wizard again.

No curses: the Windows release machine has no curses module in CPython, and a
release tool is the wrong place to need a terminal library.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import odver                      # noqa: E402
import release as release_tool    # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RULE = "-" * 74
TEST_WORKFLOW = "test.yml"

# What `release.py sdk` bumps. A change to any of these since the last Gearbox
# tag is an ABI change, which decides the order the two releases go in.
ABI_PATHS = ("sdk/abi.json", "src/mods/ModPackage.h", "src/mods/ModPackage.cpp")


def git(*arguments, check=False):
    result = subprocess.run(["git"] + list(arguments), cwd=ROOT,
                            capture_output=True, text=True)
    if check and result.returncode != 0:
        raise RuntimeError(f"git {' '.join(arguments)}: {result.stderr.strip()}")
    return result.stdout.strip()


def gh_json(*arguments):
    """Run one gh command for JSON. Return None when gh cannot answer.

    None is not an answer about the repository, so every caller treats None as
    "unknown" and says so on the screen rather than reporting a green run.
    """
    if not shutil.which("gh"):
        return None
    result = subprocess.run(["gh"] + list(arguments), cwd=ROOT,
                            capture_output=True, text=True)
    if result.returncode != 0:
        return None
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError:
        return None


# ------------------------------------------------------------------ state ---

def game_state():
    current = odver.read()
    tagged = release_tool.game_tag_exists(current)
    # release.py's own rule: bump only if what the tree says is already out,
    # so a withdrawn release is re-cut at its own number instead of leaving a
    # hole in the numbering.
    nxt = current.bump() if tagged else current
    return current, nxt, tagged


def sdk_state():
    major, minor = release_tool.sdk_version()
    return major, minor, release_tool.sdk_tag_exists(major, minor)


def abi_changed_since_tag(major, minor):
    """Whether the ABI moved since gearbox-vMAJOR.MINOR. None when untagged."""
    tag = f"gearbox-v{major}.{minor}"
    if not git("tag", "--list", tag):
        return None
    existing = [path for path in ABI_PATHS if os.path.exists(os.path.join(ROOT, path))]
    result = subprocess.run(["git", "diff", "--quiet", f"{tag}..HEAD", "--"] + existing,
                            cwd=ROOT, capture_output=True, text=True)
    return result.returncode != 0


def test_run_state(no_network):
    """The newest `Test` run on main: its conclusion, commit and failing jobs."""
    if no_network:
        return {"known": False, "why": "--no-network"}
    runs = gh_json("run", "list", "--workflow", TEST_WORKFLOW, "--branch", "main",
                   "--limit", "1", "--json", "status,conclusion,headSha,url,databaseId")
    if not runs:
        return {"known": False, "why": "gh could not answer"}
    run = runs[0]
    state = {"known": True, "status": run.get("status"),
             "conclusion": run.get("conclusion"), "sha": run.get("headSha") or "",
             "url": run.get("url"), "failed": []}
    if state["conclusion"] not in (None, "success"):
        detail = gh_json("run", "view", str(run["databaseId"]), "--json", "jobs")
        for job in (detail or {}).get("jobs", []):
            if job.get("conclusion") not in (None, "success", "skipped"):
                state["failed"].append(f"{job.get('name')} ({job.get('conclusion')})")
    return state


def notes_path(kind, version):
    """The notes file this release would use, by the naming already in docs/."""
    name = f"release-notes-{'game' if kind == 'game' else 'gearbox'}-{version}.md"
    return os.path.join("docs", name)


def gather(no_network):
    current, nxt, game_tagged = game_state()
    major, minor, sdk_tagged = sdk_state()
    head = git("rev-parse", "HEAD")
    return {
        "game": current, "next_game": nxt, "game_tagged": game_tagged,
        "sdk": (major, minor), "sdk_tagged": sdk_tagged,
        "next_sdk": (major, minor + 1) if sdk_tagged else (major, minor),
        "abi_changed": abi_changed_since_tag(major, minor),
        "dirty": [line for line in git("status", "--porcelain").splitlines() if line],
        "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "head": head,
        "test": test_run_state(no_network),
    }


# ------------------------------------------------------------------ rules ---

def blockers(facts, kind):
    """Reasons this release must not be cut now, each naming what to do."""
    found = []
    version = facts["next_game"] if kind == "game" else "%d.%d" % facts["next_sdk"]

    if facts["branch"] != "main":
        found.append(f"On branch {facts['branch']}, not main. A release is cut from main.")
    if facts["dirty"]:
        found.append(f"{len(facts['dirty'])} uncommitted change(s). A release has to be "
                     f"reproducible from its tag, and cannot be if it was cut from "
                     f"uncommitted work. release.py refuses this too.")

    test = facts["test"]
    if not test["known"]:
        found.append(f"The state of Test on main is unknown ({test['why']}). Tagging "
                     f"before a green run is how 1.2.2a took six cuts: each failure "
                     f"hid the next. Check it before answering yes.")
    else:
        if test["status"] != "completed":
            found.append(f"Test on main is {test['status']}, not finished. Wait for the "
                         f"run rather than pushing again: on main a PENDING run is "
                         f"replaced by the next push, so pushing repeatedly means the "
                         f"full run never finishes. {test['url']}")
        elif test["conclusion"] != "success":
            detail = ", ".join(test["failed"][:6]) or test["conclusion"]
            found.append(f"Test on main concluded {test['conclusion']}: {detail}. "
                         f"Read every failing job, not one. {test['url']}")
        elif test["sha"] and not facts["head"].startswith(test["sha"][:12]) \
                and not test["sha"].startswith(facts["head"][:12]):
            found.append(f"Test on main is green at {test['sha'][:12]}, and HEAD is "
                         f"{facts['head'][:12]}. A green run proves the commit it ran "
                         f"on. Push this commit and let Test finish on it.")

    if kind == "game" and facts["abi_changed"]:
        found.append(f"The ABI moved since gearbox-v%d.%d, and release.py sdk is what "
                     f"bumps the Gearbox version in the tree. Releasing the game first "
                     f"ships a host that declares the old ABI minor while implementing "
                     f"the new one. Release the SDK first." % facts["sdk"])

    if kind == "game":
        path = notes_path("game", facts["next_game"])
        if not os.path.exists(os.path.join(ROOT, path)):
            found.append(f"No notes at {path}. release.py requires notes for a game "
                         f"release: a version nobody can tell apart from the last one "
                         f"is one nobody will install. Write that file, or answer the "
                         f"notes question with - to type them.")
    return found, version


# ----------------------------------------------------------------- screen ---

def draw(facts):
    test = facts["test"]
    if not test["known"]:
        test_line = f"unknown ({test['why']})"
    elif test["status"] != "completed":
        test_line = f"{test['status']} at {test['sha'][:12]}"
    elif test["conclusion"] == "success":
        same = (test["sha"][:12] == facts["head"][:12])
        test_line = f"green at {test['sha'][:12]}" + ("" if same else "  NOT this commit")
    else:
        test_line = f"{test['conclusion']} ({len(test['failed'])} job(s) failing)"

    abi = {True: "yes -- SDK goes first", False: "no", None: "unknown (no tag)"}[facts["abi_changed"]]

    rows = [
        ("Branch", facts["branch"]),
        ("HEAD", facts["head"][:12]),
        ("Uncommitted changes", str(len(facts["dirty"]))),
        ("Game version in the tree", f"{facts['game']}"
                                    f"{'  (tagged)' if facts['game_tagged'] else '  (untagged)'}"),
        ("Game release would be", f"{facts['next_game']}"),
        ("Gearbox SDK version", "%d.%d%s" % (*facts["sdk"],
                                             "  (tagged)" if facts["sdk_tagged"] else "  (untagged)")),
        ("SDK release would be", "%d.%d" % facts["next_sdk"]),
        ("ABI moved since that tag", abi),
        ("Test on main", test_line),
    ]
    width = max(len(label) for label, _ in rows)
    print()
    print(RULE)
    print("  Open Doctrines release")
    print(RULE)
    for label, value in rows:
        print(f"  {label.ljust(width)}  {value}")
    print(RULE)
    print("  1. Release the Gearbox SDK  (gearbox-v%d.%d)" % facts["next_sdk"])
    print(f"  2. Release the game         (v{facts['next_game']})")
    print("  3. Dry run of either, which changes nothing")
    print("  4. Refresh")
    print("  q. Quit")
    print(RULE)


def ask(question, default=False):
    suffix = "[Y/n]" if default else "[y/N]"
    try:
        answer = input(f"{question} {suffix} ").strip().lower()
    except EOFError:
        return False
    return default if not answer else answer in ("y", "yes")


def prompt(question, default=""):
    try:
        answer = input(f"{question} ").strip()
    except EOFError:
        return default
    return answer or default


def build_command(facts, kind, dry_run):
    """Assemble the release.py invocation, asking only what release.py needs."""
    command = [sys.executable, "tools/release.py", kind]

    if kind == "game":
        path = notes_path("game", facts["next_game"])
        if os.path.exists(os.path.join(ROOT, path)):
            command += ["--notes", path]
        else:
            answer = prompt(f"  Notes file for {facts['next_game']} "
                            f"(- to type them, empty to let release.py decide):")
            if answer:
                command += ["--notes", answer]
        if ask("  Start a new update line rather than a patch (--new-update)?"):
            command.append("--new-update")
        state = prompt("  Switch release state a/b/r/snapshot (empty to keep):")
        if state:
            command += ["--switch-state", state]
    else:
        path = notes_path("sdk", "%d.%d" % facts["next_sdk"])
        if os.path.exists(os.path.join(ROOT, path)):
            command += ["--notes", path]
        if ask("  Raise the MAJOR Gearbox version rather than the minor (--major)?"):
            command.append("--major")

    if dry_run:
        command.append("--dry-run")
    elif not ask("  Push the tag when it is cut? Nothing releases without the push.",
                 default=True):
        command.append("--no-push")
    return command


def act(facts, kind, dry_run):
    found, version = blockers(facts, kind)
    heading = f"{'Dry run of the' if dry_run else 'Release the'} " \
              f"{'game' if kind == 'game' else 'Gearbox SDK'} {version}"
    print(f"\n{heading}")

    if found:
        print(f"  {len(found)} reason(s) not to:")
        for reason in found:
            print(f"    - {reason}")
        if not dry_run:
            print("  Refusing. A dry run (3) is available while these stand.")
            return
        print("  A dry run changes nothing, so it carries on.")

    command = build_command(facts, kind, dry_run)
    print(f"\n  About to run:\n    $ {' '.join(command)}")
    if not ask("  Run it?"):
        print("  Nothing was run.")
        return
    subprocess.run(command, cwd=ROOT)


# ------------------------------------------------------------------ check ---

def self_check():
    """Prove the wizard can still see what it claims to check. No network.

    Everything below is a way this tool can go quiet rather than wrong, which
    is the failure mode worth a check: a release that silently stops being
    gated looks exactly like a release that passed.

    release.py has already had this exact bug. set_sdk_version() substituted
    the host's Gearbox constants in ModPackage.cpp; the constants moved to the
    header for Gearbox 1.1, the substitution matched nothing, and the release
    looked clean. ABI_PATHS here is the same kind of hardcoded path list, and
    an ABI change it cannot see is an SDK-before-game rule that never fires.
    """
    problems = []

    existing = [path for path in ABI_PATHS if os.path.exists(os.path.join(ROOT, path))]
    if not existing:
        problems.append(f"None of {', '.join(ABI_PATHS)} exists, so abi_changed_since_tag "
                        f"diffs an empty path list, always answers no, and the "
                        f"SDK-before-game rule never fires. Point ABI_PATHS at wherever "
                        f"the ABI and the host's Gearbox constants now live.")
    if not any(path.endswith("abi.json") for path in existing):
        problems.append("sdk/abi.json is not among the ABI paths that exist, and it is "
                        "the file release.py sdk rewrites.")

    for name in ("game_tag_exists", "sdk_tag_exists", "sdk_version"):
        if not callable(getattr(release_tool, name, None)):
            problems.append(f"tools/release.py no longer provides {name}(), which this "
                            f"wizard reads the version state through.")

    for name in ("read", "write"):
        if not callable(getattr(odver, name, None)):
            problems.append(f"tools/odver.py no longer provides {name}().")

    # The notes filename convention. release.py takes --notes as a path, so a
    # convention this tool guesses wrongly means it silently stops offering the
    # notes file and asks instead -- quieter than a failure, and the reason a
    # version ships with the previous version's notes.
    docs = os.path.join(ROOT, "docs")
    if os.path.isdir(docs):
        for kind, prefix in (("game", "release-notes-game-"), ("sdk", "release-notes-gearbox-")):
            shipped = [name for name in os.listdir(docs) if name.startswith(prefix)]
            if not shipped:
                continue
            version = shipped[0][len(prefix):-len(".md")]
            guess = notes_path(kind, version)
            if os.path.basename(guess) != shipped[0]:
                problems.append(f"notes_path({kind!r}) builds {os.path.basename(guess)} "
                                f"and docs/ holds {shipped[0]}.")

    # Every refusal, driven. A rule that cannot fire is not a rule.
    facts = {"game": odver.read(), "next_game": odver.read(), "game_tagged": True,
             "sdk": (1, 0), "sdk_tagged": True, "next_sdk": (1, 1),
             "abi_changed": True, "dirty": [" M src/Game.cpp"], "branch": "feature",
             "head": "a" * 40,
             "test": {"known": True, "status": "completed", "conclusion": "failure",
                      "sha": "b" * 40, "url": "", "failed": ["linux-x64 (failure)"]}}
    reasons, _ = blockers(facts, "game")
    for wanted in ("not main", "uncommitted", "concluded failure", "ABI moved"):
        if not any(wanted in reason for reason in reasons):
            problems.append(f"blockers() did not refuse a release for {wanted!r}.")

    clean = dict(facts, dirty=[], branch="main", abi_changed=False,
                 test={"known": True, "status": "completed", "conclusion": "success",
                       "sha": "a" * 40, "url": "", "failed": []})
    reasons, _ = blockers(clean, "sdk")
    if reasons:
        problems.append(f"blockers() refused an SDK release with nothing wrong: {reasons}")

    if problems:
        for problem in problems:
            print(f"FAIL  {problem}")
        return 1
    print(f"ok    the release wizard sees {len(existing)} ABI path(s), release.py's "
          f"version state, and every refusal fires")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Walk through an Open Doctrines release.")
    parser.add_argument("--no-network", action="store_true",
                        help="skip the gh queries, which leaves Test on main unknown")
    parser.add_argument("--check", action="store_true",
                        help="prove the wizard can still see what it checks, and exit")
    arguments = parser.parse_args()

    if arguments.check:
        return self_check()

    while True:
        facts = gather(arguments.no_network)
        draw(facts)
        choice = prompt("  Choose:").lower()
        if choice in ("q", "quit", "exit"):
            return 0
        if choice == "1":
            act(facts, "sdk", False)
        elif choice == "2":
            act(facts, "game", False)
        elif choice == "3":
            kind = "sdk" if prompt("  Dry run which, sdk or game?").startswith("s") else "game"
            act(facts, kind, True)
        elif choice == "4":
            continue
        else:
            print("  1, 2, 3, 4 or q.")


if __name__ == "__main__":
    sys.exit(main())
