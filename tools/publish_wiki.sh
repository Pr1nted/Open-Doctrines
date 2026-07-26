#!/usr/bin/env bash
# Publish wiki/ to the repository's Wiki tab.
#
#   tools/publish_wiki.sh
#
# GitHub's wiki is a SEPARATE git repository (…/Open-Doctrines.wiki.git), which
# is why a normal push does not carry it and why the pages are not reachable
# from the Code tab. This regenerates the pages and pushes them there.
#
# BEFORE THE FIRST RUN, once:
#
#   1. Repository -> Settings -> General -> Features -> tick "Wikis".
#   2. Open the Wiki tab and click "Create the first page", then Save.
#      GitHub does not create the wiki repository until a page exists, so
#      cloning before that step fails with a 404 that looks like a permissions
#      problem but is not.
#
# After that this script is the whole workflow.
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

REMOTE="${WIKI_REMOTE:-$(git -C "$root" remote get-url origin | sed 's/\.git$//').wiki.git}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "==> regenerating wiki/ from sdk/abi.json"
python3 "$root/tools/gen_wiki.py"

echo "==> cloning $REMOTE"
if ! git clone --quiet "$REMOTE" "$work/wiki" 2>"$work/err"; then
    echo
    echo "Could not clone the wiki repository." >&2
    sed 's/^/    /' "$work/err" >&2
    echo >&2
    echo "Almost always one of two things:" >&2
    echo "  - Wikis are not enabled: Settings -> General -> Features -> Wikis." >&2
    echo "  - The wiki has no pages yet: open the Wiki tab and create one, then" >&2
    echo "    re-run. GitHub does not create the repository until a page exists." >&2
    exit 1
fi

cp "$root"/wiki/*.md "$work/wiki/"
cd "$work/wiki"

if git diff --quiet && git diff --cached --quiet; then
    echo "==> wiki is already up to date"
    exit 0
fi

git add -A
# Same authorship convention as the main repository.
git -c user.name="Pr1nted" -c user.email="" \
    commit --quiet --author "Pr1nted <>" -m "Update Gearbox mod wiki"
git push --quiet

echo "==> published"
echo "    $(echo "$REMOTE" | sed 's/\.wiki\.git$//' )/wiki"
