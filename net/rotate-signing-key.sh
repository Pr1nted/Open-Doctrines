#!/usr/bin/env bash
# Rotate the Ed25519 signing key. Nothing else.
#
# WHY THIS IS NOT PART OF setup.sh
#
# setup.sh generates all five secrets together and then refuses to touch any of
# them if IDENT_KEY already exists -- correctly, because rotating IDENT_KEY or
# PAIRWISE_KEY is unrecoverable. But that guard is all-or-nothing, so on a live
# deployment there was no way to rotate the signing key at all, even though
# README.md says it can be rotated. This is that missing path, kept separate so
# it cannot be mistaken for first-time setup.
#
# It writes exactly two secrets, named literally below. IDENT_KEY, PAIRWISE_KEY
# and ADMIN_SECRET are never referenced by this script, which is the point: a
# script that cannot name them cannot rotate them by accident.
#
# WHAT ROTATING COSTS
#
#   - every signed-in player is signed out and must sign in again
#   - every join ticket in flight becomes invalid (they last 120s anyway)
#   - EVERY SERVER CREDENTIAL becomes invalid; each host must register again
#   - a game in progress loses its players at the next authenticated action
#
# WHAT IT BUYS
#
# It is the only thing that invalidates a leaked credential. verifyServerCredential
# checks the signature, the issuer, the audience and the expiry -- there is no
# revocation list -- so a leaked token stays valid until it expires, which for
# a server credential is years. Rotating the key is the revocation.
#
#   net/rotate-signing-key.sh            rotate, after confirming
#   net/rotate-signing-key.sh --dry-run  show what would happen, change nothing
set -euo pipefail

cd "$(dirname "$0")"

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
info() { printf '  %s\n' "$1"; }
warn() { printf '  \033[33m%s\033[0m\n' "$1"; }
die()  { printf '  \033[31m%s\033[0m\n' "$1" >&2; exit 1; }

DRY=0
[ "${1:-}" = "--dry-run" ] && DRY=1

wr() { npx --no-install wrangler "$@"; }

command -v npx >/dev/null || die "npx is not installed"
wr whoami >/dev/null 2>&1 || die "wrangler is not logged in; run: npx wrangler login"

bold "Rotating the Ed25519 signing key"
echo

# Refuse on a deployment that has never been set up: rotating a key that does
# not exist is setup.sh's job, and doing it here would leave IDENT_KEY and
# PAIRWISE_KEY unset while the signing key worked -- a half-configured Worker
# that fails later and further from the cause.
EXISTING="$(wr secret list 2>/dev/null || echo '[]')"
if ! printf '%s' "$EXISTING" | grep -q 'IDENT_KEY'; then
    die "IDENT_KEY is not set, so this deployment has never been set up. Run setup.sh."
fi
if ! printf '%s' "$EXISTING" | grep -q 'ED25519_PRIVATE_KEY'; then
    die "ED25519_PRIVATE_KEY is not set. Run setup.sh."
fi
info "Found an existing deployment. IDENT_KEY and PAIRWISE_KEY will not be touched."

if [ "$DRY" = 1 ]; then
    echo
    warn "--dry-run: nothing was changed."
    info "A real run would replace ED25519_PRIVATE_KEY and ED25519_PUBLIC_JWK,"
    info "signing out every player and invalidating every server credential."
    exit 0
fi

echo
warn "This signs out EVERY player and invalidates EVERY server credential."
warn "Hosts must re-register. Games in progress will drop their players."
echo
read -r -p "  Type ROTATE to continue: " CONFIRM
[ "$CONFIRM" = "ROTATE" ] || die "Not confirmed; nothing was changed."

echo
info "Generating a new Ed25519 keypair…"
# Generated and piped straight through: the private key is never written to
# disk, never printed, and never placed in a shell variable that survives this
# process. Same approach as setup.sh.
KEYS="$(node -e '
  const { webcrypto: c } = require("node:crypto");
  c.subtle.generateKey("Ed25519", true, ["sign", "verify"]).then(async (k) => {
    console.log(JSON.stringify({
      priv: JSON.stringify(await c.subtle.exportKey("jwk", k.privateKey)),
      pub:  JSON.stringify(await c.subtle.exportKey("jwk", k.publicKey)),
    }));
  }).catch((e) => { console.error(e); process.exit(1); });
')" || die "your node build does not support Ed25519 (needs node 18+)"

read_key() { printf '%s' "$KEYS" | node -e '
  let s = ""; process.stdin.on("data", (d) => s += d)
    .on("end", () => process.stdout.write(JSON.parse(s)[process.argv[1]]));
' "$1"; }

info "Writing ED25519_PRIVATE_KEY…"
printf '%s' "$(read_key priv)" | wr secret put ED25519_PRIVATE_KEY >/dev/null \
    || die "could not write ED25519_PRIVATE_KEY"
info "Writing ED25519_PUBLIC_JWK…"
printf '%s' "$(read_key pub)" | wr secret put ED25519_PUBLIC_JWK >/dev/null \
    || die "could not write ED25519_PUBLIC_JWK"

unset KEYS

echo
bold "Rotated."
info "Every token signed by the old key is now rejected, including any that"
info "leaked. Sign in again in the game, and re-register any server you host"
info "(Multiplayer > Host a game does this for you)."
echo
info "The public half is served at \$ISSUER/.well-known/od-keys.json; a client"
info "or relay holding a cached copy of the old one will refresh on its next"
info "miss. See net/README.md, 'Key rotation'."
