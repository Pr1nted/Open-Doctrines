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
rm -rf "$out" && mkdir -p "$out/play"

# ── THE GAME IS AT /play/, AND THE SITE IS AT THE ROOT ──
#
# It used to be the other way round. The cost of moving it is one thing and it
# is worth naming: Discord's URL mapping points / at this host, so an Activity
# launched from a voice channel loads the ROOT -- which is now a landing page,
# not the game. The site's index.html forwards it on when it sees both
# frame_id and instance_id, using the same test entry.mjs uses, so no mapping
# in anybody's developer portal has to change. If that script is ever removed,
# the Activity breaks and nothing here will say so.
cp build-web/index.html \
   build-web/OpenDoctrines.js \
   build-web/OpenDoctrines.wasm \
   build-web/OpenDoctrines.data "$out/play/"
cp packaging/web/_headers "$out/_headers"
[ -f packaging/web/favicon.png ] && cp packaging/web/favicon.png "$out/"
[ -f packaging/web/favicon.png ] && cp packaging/web/favicon.png "$out/play/"

# The site itself: a handful of static pages sharing one stylesheet.
cp packaging/web/site/index.html packaging/web/site/classroom.html \
   packaging/web/site/cookies.html packaging/web/site/press.html \
   packaging/web/site/site.css \
   packaging/web/site/analytics.js packaging/web/site/robots.txt \
   packaging/web/site/sitemap.xml "$out/"

# ANALYTICS ARE SITE-ONLY, AND THAT IS A PROMISE MADE IN WRITING. The cookie
# policy and net/PRIVACY.md both say /play/ is excluded, so a stray copy of
# analytics.js into the game directory would make a published policy false.
test ! -e "$out/play/analytics.js" || {
    echo "analytics.js must not be staged into play/ -- the policies say it is not there" >&2
    exit 1
}
mkdir -p "$out/img"
cp docs/img/timelapse-political.gif "$out/img/timelapse.gif"

# ── THE LINK CARD, SERVED FROM THE ROOT FOR EVERY PAGE ──
#
# One image for the whole site: the og:image tags in every page point at this
# absolute URL, including /play/, so a join link pasted into a chat draws the
# same card as the front page. Copied from docs/itch/ rather than kept a second
# time under packaging/ -- it is 590 KB and one copy is enough.
#
# Fail loudly. A missing card is invisible in testing (the card still renders,
# just blank) and would only show up as links that quietly look broken.
[ -f docs/itch/banner-github-social.png ] || {
    echo "the link card is missing: docs/itch/banner-github-social.png" >&2
    exit 1
}
cp docs/itch/banner-github-social.png "$out/card.png"

# ── THE PRESS KIT'S ASSETS ──
#
# Curated, not the whole of docs/img: menu screens sell nothing and a press kit
# that leads with a black screen and three buttons reads as carelessness. The
# names here are the names /press links to, so a rename in one place has to be a
# rename in both -- hence the loop failing loudly rather than skipping.
mkdir -p "$out/press"
for shot in world-map province economy research policies comms tutorial \
            multiplayer mods; do
    [ -f "docs/img/$shot.png" ] || {
        echo "press kit: docs/img/$shot.png is missing but /press links to it" >&2
        exit 1
    }
    cp "docs/img/$shot.png" "$out/press/$shot.png"
done
for reel in timelapse-political timelapse-population timelapse-troops; do
    [ -f "docs/img/$reel.gif" ] || {
        echo "press kit: docs/img/$reel.gif is missing but /press links to it" >&2
        exit 1
    }
    cp "docs/img/$reel.gif" "$out/press/$reel.gif"
done
cp docs/itch/banner-github-social.png "$out/press/logo-wide.png"
cp docs/itch/cover-titled.png          "$out/press/cover.png"
echo "  press kit: $(du -sh "$out/press" | cut -f1) of images"

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
cp -R build-web/data "$out/play/data"

# Which is also why this is here. With a 404.html at the root, Pages returns a
# real 404 for a path it does not have, and a missing file fails as a missing
# file rather than as mysteriously corrupt data.
cp packaging/web/404.html "$out/404.html"

# Name the file whose absence broke it, rather than trusting the copy above.
test -s "$out/play/data/STDmaps/map.odmap" || {
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
cp packaging/web/discord/discord-sdk.js "$out/play/"
echo "  discord-sdk.js: $(( $(wc -c < "$out/play/discord-sdk.js") / 1024 )) KB"

# What a player actually downloads, which is not what `ls` says.
raw=$(du -ck "$out"/play/OpenDoctrines.* | tail -1 | cut -f1)
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
# ── THE WINDOW IS PER-CALL, BECAUSE 400 BYTES SILENTLY FAILED A GOOD DEPLOY ──
#
# This read a fixed first 400 bytes. The Activity-redirect check looks for
# `frame_id`, which lives at byte 1249 of a correct index.html -- so that check
# reported the redirect missing against a site that was serving it perfectly,
# and the whole deploy exited 1 on a deploy with nothing wrong with it.
#
# Kept small by DEFAULT rather than made large for everyone: the map probe
# asserts a zip's leading PK, and every local file header inside a zip is also
# PK, so widening that one would turn "starts with a zip" into "contains the
# letters PK somewhere", which a truncated file would still pass. Callers that
# need to see further into a page ask for it.
probe() {                       # probe <url> [--bytes N] <grep-args...>
    local url="$1"; shift
    local window=400
    if [ "${1:-}" = "--bytes" ]; then window="$2"; shift 2; fi
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
            if head -c "$window" "$tmp" | grep -qai "$@"; then rm -f "$tmp"; return 0; fi
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
# The root is the SITE now, and /play/ is the game. Both are worth asking for
# by name: a staging mistake that swapped them would leave the game reachable
# and the landing page a 404, or worse, the Activity redirect missing from a
# root that looks fine in a browser.
if probe "$site/" 'OpenDoctrines' && probe "$site/" --bytes 8192 'frame_id'; then
    echo "  ok    the root serves the site, with the Activity redirect in it"
else
    echo "  FAIL  the root is not the landing page, or the Activity redirect is missing" >&2
    echo "        an Activity launched from Discord would land on a page that never forwards it" >&2
    fail=1
fi

if probe "$site/play/" 'OpenDoctrines'; then
    echo "  ok    /play/ serves the game"
else
    echo "  FAIL  /play/ is not serving the game shell" >&2; fail=1
fi

# ── THE LINK CARD, CHECKED WHERE IT IS EASIEST TO LOSE ──
#
# /play/ is the one that matters and the one that breaks. Its tags live in
# shell.html, which reaches the site only through a web RELINK -- so editing the
# shell and redeploying without rebuilding leaves the site fine, the game fine,
# and every join link pasted into a chat still bare text. Nothing else in this
# script would notice.
for page in "" "play/" "classroom" "press"; do
    if probe "$site/$page" --bytes 8192 'og:image'; then
        echo "  ok    /$page has a link card"
    else
        echo "  FAIL  /$page has no og:image -- links to it render as bare text" >&2
        [ -n "$page" ] || echo "        (if only /play/ fails: the web build is stale, relink it)" >&2
        fail=1
    fi
done

# The card itself, asserted to BE a PNG rather than merely to exist: Pages
# answers 200 with index.html for anything missing, so "it downloads" proves
# nothing at all here.
if probe "$site/card.png" -e 'PNG'; then
    echo "  ok    the link card image is served as a PNG"
else
    echo "  FAIL  /card.png is not a PNG -- every card will draw blank" >&2; fail=1
fi

if probe "$site/classroom" '<!doctype html'; then
    echo "  ok    /classroom renders"
else
    echo "  FAIL  /classroom is not being served" >&2; fail=1
fi

# The press kit, and one of the images it links to. The page rendering proves
# nothing about the images: they are copied by a separate loop above, and a
# press kit whose screenshots are all broken is worse than no press kit.
if probe "$site/press" '<!doctype html'; then
    echo "  ok    /press renders"
else
    echo "  FAIL  /press is not being served" >&2; fail=1
fi
if probe "$site/press/world-map.png" -e 'PNG'; then
    echo "  ok    the press screenshots are served as images"
else
    echo "  FAIL  /press/world-map.png is not a PNG -- the press kit images are missing" >&2; fail=1
fi

if probe "$site/play/data/STDmaps/map.odmap" -e 'PK'; then
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
