#!/usr/bin/env bash
# Copy the server binary somewhere a concurrent rebuild cannot reach it.
#
# WHY (journal 531). A 32-run bench lost 3 of its 16 seeds because the other
# session rebuilt cmake-build-debug/OpenDoctrinesServer half way through, and
# an A/B is only quotable if both arms ran on one binary. Hashing the ends of a
# run tells you it happened; it does not stop it.
#
# WHERE, and this is the one constraint. The server resolves game data at
# <bindir>/../data, so a copy in /tmp or a scratchpad dies with "no data
# directory" -- journal 510 lost a run to exactly that. Any DIRECT CHILD of the
# repo root has the repo's data/ as its ../data, so a plain cp into a sibling
# build directory is all it takes. cmake-build-*/ is already gitignored.
#
# Usage:  PIN=$(tools/pin_server.sh) && "$PIN" --eval-ai 1 400 <seed> 3 ...
#         tools/pin_server.sh --hash     # print the pinned binary's sha256
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
pin_dir="$root/cmake-build-pin"
pin="$pin_dir/OpenDoctrinesServer"

if [ "${1:-}" = "--hash" ]; then
    [ -x "$pin" ] || { echo "no pinned binary at $pin; run $0 first" >&2; exit 1; }
    shasum -a 256 "$pin" | cut -d' ' -f1
    exit 0
fi

src="${1:-$root/cmake-build-debug/OpenDoctrinesServer}"
[ -x "$src" ] || { echo "no server binary at $src" >&2; exit 1; }
mkdir -p "$pin_dir"
# cp, not ln: a hard link would still be one inode with the build tree, and a
# symlink resolves <bindir> through to the original directory, which puts
# ../data back where the rebuild lives.
cp -f "$src" "$pin"
echo "$pin"
