#!/usr/bin/env bash
# Create the achievement-grant keypair. Once per deployment, ever.
#
# WHY ITS OWN SCRIPT
#
# This key is the opposite of the session key. rotate-signing-key.sh exists
# because rotating the SESSION key is how a leaked credential is revoked; this
# key signs achievements, which should verify for as long as the game exists.
# So it is created once, never by setup.sh's all-or-nothing block, and never by
# the rotation script. See src/achievements/grant.ts.
#
# WHAT IT DOES
#
#   1. generates an Ed25519 keypair locally (node, nothing leaves this machine)
#   2. `wrangler secret put` ACHIEVEMENT_PRIVATE_KEY and ACHIEVEMENT_PUBLIC_JWK
#   3. prints the RAW public key: that string goes into the game and launcher
#      builds as -DOD_ACHIEVEMENT_KEYS=<it>, and into the repository variable
#      OD_ACHIEVEMENT_KEYS that the release workflows read.
#
# If the key ever has to change (it leaked), run with --rotate, and PREPEND the
# new public key to OD_ACHIEVEMENT_KEYS rather than replacing it: grants
# already issued under the old key must keep verifying.
#
#   net/setup-achievement-key.sh            create, refusing if one exists
#   net/setup-achievement-key.sh --rotate   replace (read the paragraph above)
#   net/setup-achievement-key.sh --dry-run  generate and print, set nothing
set -euo pipefail
cd "$(dirname "$0")"

mode="${1:-create}"
if [[ "$mode" == "create" ]] && npx wrangler secret list 2>/dev/null | grep -q '"ACHIEVEMENT_PRIVATE_KEY"'; then
    echo "ACHIEVEMENT_PRIVATE_KEY already exists. It is meant to be permanent."
    echo "Use --rotate only if it leaked, and keep the old public key in OD_ACHIEVEMENT_KEYS."
    exit 1
fi

pair="$(node -e '
const c = require("crypto");
const k = c.generateKeyPairSync("ed25519");
const priv = k.privateKey.export({ format: "jwk" });
const pub = k.publicKey.export({ format: "jwk" });
process.stdout.write(JSON.stringify({ priv, pub }));
')"
priv="$(node -e 'const p=JSON.parse(process.argv[1]);process.stdout.write(JSON.stringify({kty:p.priv.kty,crv:p.priv.crv,x:p.priv.x,d:p.priv.d}))' "$pair")"
pub="$(node -e 'const p=JSON.parse(process.argv[1]);process.stdout.write(JSON.stringify({kty:p.pub.kty,crv:p.pub.crv,x:p.pub.x}))' "$pair")"
raw="$(node -e 'process.stdout.write(JSON.parse(process.argv[1]).pub.x)' "$pair")"

if [[ "$mode" == "--dry-run" ]]; then
    echo "public (raw, base64url): $raw"
    exit 0
fi

printf '%s' "$priv" | npx wrangler secret put ACHIEVEMENT_PRIVATE_KEY
printf '%s' "$pub"  | npx wrangler secret put ACHIEVEMENT_PUBLIC_JWK

echo
echo "Done. Bake this into every build of the game and of Unifico:"
echo
echo "    OD_ACHIEVEMENT_KEYS=$raw"
echo
echo "  gh variable set OD_ACHIEVEMENT_KEYS --repo Pr1nted/Open-Doctrines --body \"$raw\""
echo "  gh variable set OD_ACHIEVEMENT_KEYS --repo Pr1nted/Unifico       --body \"$raw\""
[[ "$mode" == "--rotate" ]] && echo "  (ROTATING: prepend, e.g. \"$raw,<old key>\")"
