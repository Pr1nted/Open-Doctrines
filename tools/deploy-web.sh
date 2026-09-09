#!/usr/bin/env bash
# Build the web version and put it on Cloudflare Pages.
#
#   tools/deploy-web.sh [project-name]
#
# Authentication is wrangler's own: it opens a browser the first time and keeps
# the token itself. Nothing here reads, prints or stores a credential -- which
# is also why this is a script you run rather than something CI does until you
# have decided to give CI a token.
#
# WHY PAGES. The account service is already a Worker on the same account, so
# the Activity's one URL mapping and its API end up on infrastructure you
# already administer. Any static host with HTTPS would do.

set -euo pipefail
cd "$(dirname "$0")/.."

project="${1:-opendoctrines}"

command -v emcmake >/dev/null || {
    echo "emcmake not found. Install and activate emsdk first." >&2; exit 1; }

echo "== building =="
# The same flags the release workflow uses, so what is deployed is what is
# shipped -- including the account service, without which sign-in is missing,
# and the Discord application id, without which there is no rich presence.
emcmake cmake -B build-web -DCMAKE_BUILD_TYPE=Release \
    -DOD_ACCOUNT_ISSUER=https://opendoctrines-net.opendoctrines.workers.dev \
    -DOD_DISCORD_APP_ID=1547303703370014830
emmake cmake --build build-web -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo "== staging =="
out=build-web/_deploy
rm -rf "$out" && mkdir -p "$out"
cp build-web/index.html \
   build-web/OpenDoctrines.js \
   build-web/OpenDoctrines.wasm \
   build-web/OpenDoctrines.data "$out/"
cp packaging/web/_headers "$out/_headers"
[ -f packaging/web/favicon.png ] && cp packaging/web/favicon.png "$out/"

# What a player actually downloads, which is not what `ls` says.
raw=$(du -ck "$out"/OpenDoctrines.* | tail -1 | cut -f1)
echo "  raw: $((raw / 1024)) MB (compressed on the wire; see the README note)"

echo "== deploying =="
npx wrangler pages deploy "$out" --project-name "$project"
