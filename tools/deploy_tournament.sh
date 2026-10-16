#!/usr/bin/env bash
# Put a version of the server on the tournament host, without a release.
#
#   tools/deploy_tournament.sh v1.3.2b          # a release
#   tools/deploy_tournament.sh <commit>         # a server hotfix
#   tools/deploy_tournament.sh --rollback <ref> # back to an earlier one
#   tools/deploy_tournament.sh --status         # what it runs now
#
# The Render service builds and deploys whatever the `tournament` branch points
# at, on every push to it (deploy/server/render.yaml: branch + autoDeployTrigger).
# Pushes to main never reach it. So a fix to the server is a commit, then this:
# no tag, no client release, no store upload -- players keep the build they
# have, because the protocol is what they share with the server and a server
# fix does not change it. A fix that DOES change what clients must run is a
# release, not a hotfix.
#
# Moving forward only, unless --rollback: a push that is not a fast-forward
# would quietly un-deploy something, which is what a rollback is FOR and what a
# hotfix must never do by accident. Refuses a dirty tree's HEAD so what is
# deployed is something that exists on the remote.
#
# A deploy restarts the server: the old instance saves and hands over, and the
# turn clock carries on. Expect about a minute with nobody hosting; with a lock
# Render killed before it was released, up to OD_SYNC_STALE_SECONDS (6 min).
set -euo pipefail
branch=tournament
remote=origin

die() { echo "deploy_tournament: $*" >&2; exit 1; }
cd "$(git rev-parse --show-toplevel)"
git fetch -q "$remote"

if [ "${1:-}" = "--status" ]; then
    if git rev-parse -q --verify "$remote/$branch" >/dev/null; then
        git log -1 --format='tournament runs %h  %s  (%cr)' "$remote/$branch"
        git describe --tags --contains "$remote/$branch" 2>/dev/null | sed 's/^/  in release: /' || true
        echo "  main is $(git rev-list --count "$remote/$branch..$remote/main") commit(s) ahead of it"
    else
        echo "no $branch branch on $remote yet"
    fi
    exit 0
fi

rollback=0
[ "${1:-}" = "--rollback" ] && { rollback=1; shift; }
ref="${1:-}"
[ -n "$ref" ] || die "name a tag or commit (see the top of this file)"
sha="$(git rev-parse -q --verify "$ref^{commit}")" || die "no such tag or commit: $ref"
git branch -r --contains "$sha" | grep -q . || die "$ref is not on $remote; push it first"

if git rev-parse -q --verify "$remote/$branch" >/dev/null; then
    cur="$(git rev-parse "$remote/$branch")"
    [ "$cur" = "$sha" ] && { echo "tournament already runs $(git log -1 --format='%h %s' "$sha")"; exit 0; }
    if [ "$rollback" = 0 ] && ! git merge-base --is-ancestor "$cur" "$sha"; then
        die "$ref does not contain what the server runs now ($(git log -1 --format=%h "$cur")).
    That would take something off the server. If that is the point, use --rollback."
    fi
    echo "tournament: $(git log -1 --format='%h %s' "$cur")"
    git log --oneline "$cur..$sha" | sed 's/^/  + /' | head -20
fi
echo "      -> $(git log -1 --format='%h %s' "$sha")"
read -r -p "Deploy this to the tournament server? [y/N] " ok
[ "$ok" = y ] || [ "$ok" = Y ] || die "not deployed"

if [ "$rollback" = 1 ]; then
    git push --force-with-lease="$branch" "$remote" "$sha:refs/heads/$branch"
else
    git push "$remote" "$sha:refs/heads/$branch"
fi
echo "pushed. Render builds it now (about 6 minutes), then hands over."
echo "Watch: the service's Logs tab, for 'session open. Join code:'."
