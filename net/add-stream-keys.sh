#!/usr/bin/env bash
# Add the streaming platforms' app credentials, without them passing through
# anybody's terminal history or a chat log.
#
# The live-now check needs an app registered with each platform. Nothing else
# does: sign-in and chat reading work without these, and a platform with no
# credential simply never shows a live badge (see src/live/lookup.ts).
#
#   Twitch   https://dev.twitch.tv/console/apps   -> client id + secret
#   YouTube  https://console.cloud.google.com     -> a Data API v3 key
#   Kick     https://kick.com/settings/developer  -> client id + secret
#
# Run it and paste each value at the prompt. They are read with `read -s`, so
# nothing is echoed, nothing lands in your shell history, and this script never
# prints them back.

set -uo pipefail
cd "$(dirname "$0")"

ask() {
    local name="$1" prompt="$2" value=""
    printf '%s: ' "$prompt"
    read -rs value
    printf '\n'
    [ -z "$value" ] && { echo "  (skipped)"; return; }
    npx wrangler secret put "$name" <<< "$value" >/dev/null 2>&1 \
        && echo "  $name set" || echo "  $name FAILED"
}

echo "Leave any prompt empty to skip that platform."
ask TWITCH_CLIENT_ID     "Twitch client id"
ask TWITCH_CLIENT_SECRET "Twitch client secret"
ask YOUTUBE_API_KEY      "YouTube Data API key"
ask KICK_CLIENT_ID       "Kick client id"
ask KICK_CLIENT_SECRET   "Kick client secret"

echo
echo "Done. To test one without deploying, put the same names in net/.dev.vars"
echo "and run:  npx wrangler dev --local"
echo "then:     curl -s localhost:8787/live -H 'content-type: application/json' \\"
echo "               -d '{\"channels\":[{\"platform\":\"twitch\",\"channel\":\"CHANNEL\"}]}'"
