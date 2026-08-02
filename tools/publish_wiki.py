#!/usr/bin/env python3
"""Push wiki/ to the repository's Wiki tab.

    python3 tools/publish_wiki.py             # publish
    python3 tools/publish_wiki.py --dry-run   # say what would change, touch nothing

The wiki is a SECOND GIT REPOSITORY that GitHub keeps beside this one, at
<repo>.wiki.git. That is the whole reason this script exists: nothing in a
normal push reaches it, so pages written here sat in the tree, correct and
reviewed and linked to from the game, while the Wiki tab stayed empty. Anything
that has to be remembered by hand eventually is not.

Two things are rewritten on the way out:

  * `[text](Page-Name.md)` becomes `[text](Page-Name)`. The in-repo copy carries
    the `.md` so the pages also work when read in the Code tab (see
    gen_wiki.py); the Wiki tab serves pages without it.
  * README.md is not published. It is the folder's front page in the Code tab;
    the wiki's front page is Home.

FIRST RUN NEEDS ONE MANUAL STEP. GitHub does not create the wiki repository
until a first page exists, and there is no API for it: open
https://github.com/Pr1nted/Open-Doctrines/wiki, click "Create the first page",
save anything at all, then run this. It is overwritten on the first publish.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_wiki import OUT, ROOT, unlink_md  # noqa: E402

# The Code tab's front page, and only that. Everything else in wiki/ is a page.
NOT_PUBLISHED = {"README.md"}


def run(cmd, cwd=None, check=True):
    r = subprocess.run(cmd, cwd=cwd, text=True, capture_output=True)
    if check and r.returncode != 0:
        sys.stderr.write((r.stdout or "") + (r.stderr or ""))
        raise SystemExit(f"failed: {' '.join(cmd)}")
    return r


def wiki_remote():
    """Where the wiki lives, derived from this checkout's own origin.

    Not hardcoded, so a fork publishes to its own wiki rather than failing on
    somebody else's. WIKI_REMOTE overrides it, which is also how the tests
    point this at a throwaway repository."""
    override = os.environ.get("WIKI_REMOTE")
    if override:
        return override
    origin = run(["git", "-C", ROOT, "remote", "get-url", "origin"]).stdout.strip()
    return re.sub(r"(\.git)?$", ".wiki.git", origin, count=1)


def wiki_url():
    """The Wiki tab a person can open, for messages that ask them to."""
    return re.sub(r"\.wiki\.git$", "/wiki", wiki_remote())


def remote_url():
    """The wiki remote, with a CI token folded in when there is one.

    GITHUB_TOKEN is how the workflow authenticates; a developer running this by
    hand has git credentials already and gets the plain URL."""
    remote = wiki_remote()
    token = os.environ.get("GITHUB_TOKEN", "")
    if not token or not remote.startswith("https://"):
        return remote
    return remote.replace("https://", f"https://x-access-token:{token}@")


def main(argv):
    dry_run = "--dry-run" in argv

    # Refuse to publish a wiki/ that no longer matches its sources. Publishing
    # is the one moment the pages stop being a file in a tree and start being
    # documentation people read, and a stale API reference is the failure this
    # whole generator exists to prevent.
    if run([sys.executable, os.path.join(ROOT, "tools", "gen_wiki.py"), "--check"],
           check=False).returncode != 0:
        raise SystemExit("wiki/ is stale -- run tools/gen_wiki.py first")

    pages = sorted(f for f in os.listdir(OUT)
                   if f.endswith(".md") and f not in NOT_PUBLISHED)
    if not pages:
        raise SystemExit(f"no pages in {OUT}")

    tmp = tempfile.mkdtemp(prefix="od-wiki-")
    try:
        clone = run(["git", "clone", "--depth", "1", remote_url(), tmp], check=False)
        if clone.returncode != 0:
            # A 404 here reads like a permissions problem and is almost never
            # one, so say what it actually is. Both causes are one click.
            sys.stderr.write("Could not clone the wiki repository.\n")
            for line in (clone.stderr or "").splitlines():
                sys.stderr.write(f"    {line}\n")
            raise SystemExit(
                "\nAlmost always one of two things:\n"
                "  - Wikis are off: Settings -> General -> Features -> Wikis.\n"
                "  - The wiki has no pages yet. GitHub does not create the\n"
                "    repository until one exists, and offers no API for it: open\n"
                f"    {wiki_url()}\n"
                "    click \"Create the first page\" and save anything at all. The\n"
                "    first publish overwrites it.")

        # Pages this script owns and the wiki still has are removed, so a page
        # deleted here does not live on there forever.
        for existing in os.listdir(tmp):
            if existing != ".git" and existing.endswith(".md"):
                os.remove(os.path.join(tmp, existing))

        for name in pages:
            body = unlink_md(open(os.path.join(OUT, name)).read())
            with open(os.path.join(tmp, name), "w") as f:
                f.write(body)

        run(["git", "add", "-A"], cwd=tmp)
        status = run(["git", "status", "--porcelain"], cwd=tmp).stdout.strip()
        if not status:
            print("wiki is already up to date")
            return 0

        print(f"{len(pages)} pages, changes:")
        print(status)
        if dry_run:
            print("\n--dry-run: nothing pushed")
            return 0

        # Attributed to whoever is publishing, not to a bot identity this
        # repository does not otherwise have. CI sets these itself.
        if os.environ.get("GITHUB_ACTIONS"):
            run(["git", "config", "user.name", "github-actions[bot]"], cwd=tmp)
            run(["git", "config", "user.email",
                 "41898282+github-actions[bot]@users.noreply.github.com"], cwd=tmp)

        head = run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT).stdout.strip()
        run(["git", "commit", "-m", f"Update wiki from {head}"], cwd=tmp)
        run(["git", "push"], cwd=tmp)
        print(f"\npublished to {wiki_url()}")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
