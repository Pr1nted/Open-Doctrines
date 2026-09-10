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

# Named once. Both of these are also stamped into the page's Discord Activity
# prologue by CMake, so a deploy that pointed the game at one service and the
# proxy mapping at another is not a thing that can be typed by accident.
issuer="https://opendoctrines-net.opendoctrines.workers.dev"
app_id="1547303703370014830"

echo "== building =="
# The same flags the release workflow uses, so what is deployed is what is
# shipped -- including the account service, without which sign-in is missing,
# and the Discord application id, without which there is no rich presence.
emcmake cmake -B build-web -DCMAKE_BUILD_TYPE=Release \
    -DOD_ACCOUNT_ISSUER="$issuer" \
    -DOD_DISCORD_APP_ID="$app_id"
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

# ── THE STREAMED HALF OF THE GAME, WHICH THIS SCRIPT USED TO LEAVE BEHIND ──
#
# Not everything is inside OpenDoctrines.data. The maps, the music, the AI
# model and the full fonts are served as ordinary files NEXT TO the page and
# fetched on demand -- that is deliberate, and it is why the preload is 10 MB
# instead of 70. This script copied four files and the headers, so every
# deploy until now put a menu online with no maps behind it.
#
# It was invisible for the worst possible reason: Cloudflare Pages answers a
# missing path with index.html and status 200, so the game asked for a 1.2 MB
# map, was handed 10 KB of HTML, and reported that the download had failed.
# Nothing was down. The file had never been uploaded.
cp -R build-web/data "$out/data"

# Which is also why this is here. With a 404.html at the root, Pages returns a
# real 404 for a path it does not have, and a missing file fails as a missing
# file rather than as mysteriously corrupt data.
cp packaging/web/404.html "$out/404.html"

# Name the file whose absence broke it, rather than trusting the copy above.
test -s "$out/data/STDmaps/map.odmap" || {
    echo "the default map is not in the upload -- the game would have no world to start" >&2
    exit 1
}

# ── THE POLICY PAGES ──
#
# Discord refuses a workers.dev URL for an application's privacy policy or
# terms -- it is a shared suffix, so anyone can hold one -- and pages.dev it
# accepts. The account service still serves both documents to the game as
# markdown; these are the same text rendered for people, generated from the
# same files, so the two cannot drift.
echo "== policy pages =="
( cd packaging/web/policies && npm ci --no-audit --no-fund && npm run build ) \
    || { echo "could not render the policy pages" >&2; exit 1; }
cp packaging/web/policies/privacy.html packaging/web/policies/terms.html "$out/"

# Discord's Embedded App SDK, bundled from the pinned version rather than
# committed as a minified blob nobody can read. The page fetches it ONLY when
# it is running inside an Activity, so this costs the itch.io and plain-web
# players nothing -- but it is staged unconditionally, because the alternative
# is a deploy that works everywhere except the one place it was built for.
#
# A failure here stops the deploy. Without this file the Activity loads, plays
# its own loading screen forever, and every network call it makes is refused
# by the proxy: a break that is invisible from here and obvious to a player.
echo "== discord sdk =="
( cd packaging/web/discord && npm ci --no-audit --no-fund && npm run build ) \
    || { echo "could not build the Discord SDK bundle" >&2; exit 1; }
cp packaging/web/discord/discord-sdk.js "$out/"
echo "  discord-sdk.js: $(( $(wc -c < "$out/discord-sdk.js") / 1024 )) KB"

# What a player actually downloads, which is not what `ls` says.
raw=$(du -ck "$out"/OpenDoctrines.* | tail -1 | cut -f1)
echo "  raw: $((raw / 1024)) MB (compressed on the wire; see the README note)"

echo "== deploying =="
npx wrangler pages deploy "$out" --project-name "$project"

# ── CHECK THE SITE, NOT THE UPLOAD ──
#
# A deploy that reports success and serves a broken game is exactly what
# happened here, twice over: the staging was wrong, and the host's 200-for-
# everything hid it. Both are only visible from outside, so ask the live URL
# for the two things a player needs and look at what actually comes back.
echo "== checking the deployed site =="
site="https://${project}.pages.dev"
fail=0

# RETRY, because a deploy is not instantly everywhere. The first version of
# these checks read each URL exactly once, moments after wrangler said
# "Deploying...", and reported the privacy policy missing while the terms were
# fine -- purely because the larger file had not propagated yet. The page was
# correct the whole time. A check that fails at random on a legal document is
# worse than no check: it is one people learn to skip past.
#
# Prints nothing until it has an answer, and gives up after ~20s so a genuinely
# broken deploy still fails rather than hanging.
probe() {                       # probe <url> <grep-args...> -> 0 if matched
    local url="$1"; shift
    local tmp i
    tmp=$(mktemp)
    for i in 1 2 3 4 5 6 7 8 9 10; do
        # TO A FILE, NOT THROUGH A PIPE INTO head.
        #
        # `curl | head -c 400` looks equivalent and is not. head closes the
        # pipe after 400 bytes, curl dies of EPIPE, and `set -o pipefail` makes
        # the whole pipeline fail even though grep matched. It only bites above
        # the pipe buffer, so the 23 KB policy pages passed and the 1.2 MB map
        # failed -- a check that reported a missing map against a site that was
        # serving it perfectly. Testing the pipeline by hand did not reproduce
        # it, because an interactive shell has no pipefail.
        if curl -fsS -o "$tmp" "$url" 2>/dev/null; then
            # -a because a map is binary, and BSD grep silently reports "no
            # match" on binary input rather than an error.
            if head -c 400 "$tmp" | grep -qai "$@"; then rm -f "$tmp"; return 0; fi
        fi
        sleep 2
    done
    rm -f "$tmp"
    return 1
}

# The map, and specifically NOT the page wearing the map's name.
# A map is a zip, so it starts PK. Asserting what it IS rather than what it is
# not: "does not look like HTML" would also pass on an empty body or a
# truncated one.
if probe "$site/data/STDmaps/map.odmap" -e 'PK'; then
    echo "  ok    the default map is served as map data"
else
    echo "  FAIL  the map URL is not returning map data -- is data/ in the upload?" >&2; fail=1
fi

# The policy pages, at the extensionless URLs given to Discord. Checked because
# a link on a public application profile that 404s is worse than no link, and
# because "does Pages serve /privacy from privacy.html" is a host behaviour
# rather than something this script can know.
for doc in privacy terms; do
    if probe "$site/$doc" '<!doctype html'; then
        echo "  ok    /$doc renders as a page"
    else
        echo "  FAIL  /$doc is not serving a rendered policy" >&2; fail=1
    fi
done

# A path that cannot exist. If this is a 200 the host is still lying, and the
# check above is the only thing standing between a bad deploy and a player.
code=$(curl -s -o /dev/null -w '%{http_code}' "$site/od-deploy-check-$$" || true)
if [ "$code" = "404" ]; then
    echo "  ok    a missing file answers 404"
else
    echo "  FAIL  a missing file answers $code, so nothing can tell one from a real file" >&2; fail=1
fi

[ "$fail" = 0 ] || { echo "the deploy went up but the site is not right" >&2; exit 1; }
echo "  the site is serving a playable build"
