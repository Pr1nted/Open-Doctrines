#!/usr/bin/env bash
# A second player on one machine.
#
# WHY THIS IS NOT JUST "RUN IT TWICE"
#
# Two copies started normally share one data directory, which means one
# account.json. The second sign-in evicts the first, and worse, both instances
# then present the SAME pseudonym -- so the lobby correctly reads the joiner as
# the host reconnecting and hands it the host's own seat. It looks like a bug in
# the lobby. It is two copies of one person.
#
# This makes a second data directory: its own account, config, servers and
# saves, with the big read-only assets symlinked rather than copied, so it costs
# kilobytes instead of a gigabyte.
#
# WHAT IT STILL CANNOT DO
#
# Give you a second IDENTITY. A psid is derived from the account you sign in
# with, so signing into both copies with the same Google account produces the
# same player twice however many directories you make. For two genuinely
# different players you need two different provider accounts -- a second
# GitHub or Discord login is the usual way. See docs/multiplayer-testing.md.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/data"
dst="${1:-$root/data-p2}"

[ -d "$src" ] || { echo "no data directory at $src" >&2; exit 1; }

mkdir -p "$dst"

# Read-only bulk: linked, never copied. A map pack is hundreds of megabytes and
# neither instance writes to it.
for entry in "$src"/*; do
    name="$(basename "$entry")"
    case "$name" in
        # Everything an instance writes to must be its OWN, or the two fight.
        account.json|config.json|servers.json|saves|tools) continue ;;
    esac
    [ -e "$dst/$name" ] || ln -s "$entry" "$dst/$name"
done

mkdir -p "$dst/saves/multiplayer"

# A fresh config, signed out, on the same account service as the first copy.
if [ ! -f "$dst/config.json" ]; then
    if [ -f "$src/config.json" ]; then
        # Carried over so the second copy talks to the same service -- but the
        # session itself is deliberately not copied.
        python3 - "$src/config.json" "$dst/config.json" <<'PY'
import json, sys
src, dst = sys.argv[1], sys.argv[2]
try:
    cfg = json.load(open(src))
except Exception:
    cfg = {}
cfg.pop("accountAgreed", None)          # the second player agrees for themselves
json.dump(cfg, open(dst, "w"), indent=2)
PY
    else
        echo '{}' > "$dst/config.json"
    fi
fi

# Never inherit a session: that is the whole point.
rm -f "$dst/account.json"

echo "Second player's data directory: $dst"
echo
echo "Start it with:"
echo "  OD_DATA_DIR='$dst' ./cmake-build-debug/OpenDoctrines.app/Contents/MacOS/OpenDoctrines"
echo
echo "Sign in there with a DIFFERENT provider account, or you will be the same"
echo "player twice and the lobby will treat the join as your own reconnect."
