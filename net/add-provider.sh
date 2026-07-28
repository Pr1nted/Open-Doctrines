#!/usr/bin/env bash
#
# Add a sign-in provider to the deployed account service.
#
#   ./add-provider.sh github
#   ./add-provider.sh google
#   ./add-provider.sh discord
#
# Prompts for the client id and secret, uploads both, then WAITS UNTIL THE
# SERVICE ACTUALLY OFFERS THE PROVIDER. That last part is the point: two
# `wrangler secret put` calls succeed whether or not the values are right, and
# without this you would find out at the consent screen instead.
#
# The secret is read with `read -s` and piped, so it never reaches your shell
# history nor a command line that `ps` could show.

set -euo pipefail
cd "$(dirname "$0")"

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
info() { printf '  %s\n' "$1"; }
warn() { printf '\033[33m  %s\033[0m\n' "$1"; }
die()  { printf '\033[31mError: %s\033[0m\n' "$1" >&2; exit 1; }

PROVIDER="${1:-}"
case "$PROVIDER" in
    google|discord|github) ;;
    *) die "usage: ./add-provider.sh google|discord|github" ;;
esac
UPPER="$(printf '%s' "$PROVIDER" | tr '[:lower:]' '[:upper:]')"

wrangler() { npx --no-install wrangler "$@"; }

ISSUER="$(grep -E '^ISSUER' wrangler.toml | sed 's/.*= *"\(.*\)"/\1/')"
[ -n "$ISSUER" ] || die "no ISSUER in wrangler.toml"

bold "Adding $PROVIDER to $ISSUER"
echo
info "Its redirect URI must be registered as EXACTLY:"
info "  $ISSUER/auth/callback/$PROVIDER"
echo
info "And the scopes must stay as src/auth/providers.ts sets them:"
info "  google: openid only   discord: identify only   github: none"
info "Widening them would collect data PRIVACY.md says we do not have."
echo

read -r -p "  ${UPPER}_CLIENT_ID: " CID
read -r -s -p "  ${UPPER}_CLIENT_SECRET: " CSEC; echo

# Trim whitespace. A trailing space or newline picked up while copying makes a
# secret that is wrong in a way nothing displays -- the provider simply answers
# "incorrect_client_credentials" much later, at the token exchange.
trim() { printf '%s' "$1" | tr -d '[:space:]'; }
CID="$(trim "$CID")"
CSEC="$(trim "$CSEC")"

[ -n "$CID" ]  || die "no client id given"
[ -n "$CSEC" ] || die "no client secret given"
info "client id ${#CID} chars, secret ${#CSEC} chars"

printf '%s' "$CID"  | wrangler secret put "${UPPER}_CLIENT_ID"     >/dev/null 2>&1 || die "could not set the client id"
printf '%s' "$CSEC" | wrangler secret put "${UPPER}_CLIENT_SECRET" >/dev/null 2>&1 || die "could not set the client secret"
info "Uploaded."

# Secrets apply without a redeploy, but the edge takes a moment to pick them up.
printf '  Waiting for the service to offer it'
for _ in $(seq 1 20); do
    if curl -s -m 8 "$ISSUER/" 2>/dev/null | grep -q "\"id\":\"$PROVIDER\""; then
        echo
        bold "  $PROVIDER is live."
        info "Open the game > Account and it will be on the sign-in list."
        exit 0
    fi
    printf '.'
    sleep 5
done

echo
warn "Set, but the service still is not offering $PROVIDER."
warn "Check the id and secret are from the same OAuth app, then re-run this."
exit 1
