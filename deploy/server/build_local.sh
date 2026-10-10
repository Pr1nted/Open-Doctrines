#!/usr/bin/env bash
# Build the server image from what git knows about, the way a hosting platform
# that clones the repository would -- not from the working directory, which in
# a developer's tree carries gigabytes of build caches, saves and local models
# that Docker would otherwise upload to its builder before failing on them.
#
#   deploy/server/build_local.sh [tag] [--platform linux/amd64]
#
# Includes uncommitted changes to tracked files and new files git is not told
# to ignore, so what you test is what you are about to commit.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
tag="${1:-od-server:dev}"; shift || true
ctx="$(mktemp -d "${TMPDIR:-/tmp}/od-server-ctx.XXXXXX")"
trap 'rm -rf "$ctx"' EXIT
cd "$root"
git ls-files --cached --others --exclude-standard -z |
    grep -zvE '^(data/(saves|llm|mods|tools|projects|custom_maps)/|data/ai/models/|web/|docs/img/)' |
    while IFS= read -r -d '' f; do
        [ -f "$f" ] || continue                       # deleted in the working tree
        mkdir -p "$ctx/$(dirname "$f")"
        cp -p "$f" "$ctx/$f"
    done
echo "context: $(du -sh "$ctx" | cut -f1)"
docker build -f "$ctx/deploy/server/Dockerfile" -t "$tag" "$@" "$ctx"
