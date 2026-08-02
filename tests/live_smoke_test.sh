#!/usr/bin/env bash
# The same connectivity check, against the REAL account service.
#
# tests/connectivity_test.sh proves a host and a client agree with each other,
# using a stand-in issuer. This proves they agree with what is actually
# deployed -- which is a different question, and the one that catches DRIFT: a
# claim renamed in the Worker, a limit tightened, a key rotated. The stand-in
# would happily keep passing through all of that.
#
# NOT part of tests/run_all.sh, on purpose:
#
#   - it needs YOUR signed-in account, because minting a join ticket requires a
#     real session token;
#   - it talks to the network, so it fails when the network does, which is not
#     a thing a test suite should report as a broken build;
#   - it creates a real (empty, immediately closed) session on the service.
#
# Run it by hand when you have changed the Worker, or before a release.
#
# Usage:
#     tests/live_smoke_test.sh [build-dir]
#
# It reads the token the game already stored at data/account.json. Nothing is
# printed from that file, and nothing is written to it.

set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build}"

step() { printf '\n=== %s ===\n' "$1"; }

account="$root/data/account.json"
if [ ! -f "$account" ]; then
    echo "No signed-in account found at data/account.json."
    echo "Sign in from the game's Account screen first -- this test mints a real"
    echo "join ticket, which needs a real session token."
    exit 1
fi

# Pulled out with python rather than grep so a reformatted file still works.
read -r ISSUER TOKEN <<EOF
$(python3 - "$account" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print("", ""); raise SystemExit
print(d.get("issuer", ""), d.get("token", ""))
PY
)
EOF

if [ -z "${ISSUER:-}" ] || [ -z "${TOKEN:-}" ]; then
    echo "data/account.json has no issuer/token. Sign in from the game first."
    exit 1
fi

# The token is a credential. It goes to the test binary in the environment, is
# never echoed, and never reaches a game server -- only the account service.
echo "issuer: $ISSUER"
echo "token:  (read from data/account.json, not shown)"

step "reachability"
if ! curl -sf --max-time 15 "$ISSUER/.well-known/od-keys.json" >/dev/null; then
    echo "Could not reach $ISSUER -- is it deployed, and is the network up?"
    exit 1
fi
echo "ok    the account service answers and publishes a key"

# The two documents the Account screen sends players to, checked against the
# copies in this tree.
#
# This is the drift the rest of the script cannot see. /terms was written,
# committed, wired into the Worker and linked from a button in the game, and
# answered 404 on the deployed service for a week -- because the route existed
# in the repository and the DEPLOYMENT predated it. Nothing failed: the tests
# pass against the source, and the source was never the thing serving players.
#
# A mismatch is a stale deployment, not a broken document. Fix it with
# `cd net && npx wrangler deploy`.
step "the documents a player agrees to"
docs_rc=0
for doc in privacy terms; do
    case "$doc" in
        privacy) repo="$root/net/PRIVACY.md" ;;
        terms)   repo="$root/net/TERMS.md" ;;
    esac
    served="$(curl -sf --max-time 15 "$ISSUER/$doc")" || {
        echo "FAIL  $ISSUER/$doc does not answer -- the deployed Worker predates it"
        docs_rc=1; continue
    }
    if [ -z "$served" ]; then
        echo "FAIL  $ISSUER/$doc is empty"
        docs_rc=1
    elif [ "$served" != "$(cat "$repo")" ]; then
        echo "FAIL  $ISSUER/$doc differs from $(basename "$repo") -- deploy the Worker"
        docs_rc=1
    else
        echo "ok    $doc matches $(basename "$repo")"
    fi
done
[ "$docs_rc" -eq 0 ] || exit 1

step "build"
cmake --build "$build" --target NetConnectTest >/dev/null || {
    echo "build failed"; exit 1;
}

step "a real join, end to end"
OD_LIVE_TOKEN="$TOKEN" "$build/NetConnectTest" "$ISSUER" live
rc=$?

if [ "$rc" -eq 0 ]; then
    printf '\nLIVE SMOKE OK\n'
else
    printf '\nLIVE SMOKE FAILED\n'
fi
exit $rc
