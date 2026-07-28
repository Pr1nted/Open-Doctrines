#!/usr/bin/env bash
#
# One-time Cloudflare setup for the OpenDoctrines account service.
#
#   cd net && ./setup.sh
#
# Safe to re-run: it skips anything already done rather than clobbering it. The
# one thing it will never do is overwrite an existing IDENT_KEY or PAIRWISE_KEY,
# because rotating either destroys every account (see README.md, "Key rotation").
#
# Secrets are piped straight into `wrangler secret put` and never printed, never
# written to a file, and never placed in a shell argument where `ps` could see
# them.

set -euo pipefail

cd "$(dirname "$0")"

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
info() { printf '  %s\n' "$1"; }
warn() { printf '\033[33m  %s\033[0m\n' "$1"; }
die()  { printf '\033[31mError: %s\033[0m\n' "$1" >&2; exit 1; }

command -v node >/dev/null || die "node is required"
[ -d node_modules ] || die "run 'npm install' first"

wrangler() { npx --no-install wrangler "$@"; }

# --------------------------------------------------------------- account ----

bold "1. Cloudflare account"
if ! wrangler whoami >/dev/null 2>&1; then
    warn "Not logged in. A browser window will open."
    wrangler login
fi
ACCOUNT="$(wrangler whoami 2>/dev/null | grep -oE '[0-9a-f]{32}' | head -1 || true)"
info "Logged in${ACCOUNT:+ (account ${ACCOUNT:0:8}…)}"

# ------------------------------------------------------- workers.dev name ----

bold "2. workers.dev subdomain"
cat <<'EOF'
  Your service URL will be <worker>.<subdomain>.workers.dev, and it is what
  every player's game connects to.

  THIS CAN ONLY BE SET ONCE. Cloudflare has no supported way to change it
  afterwards -- the dashboard link is gone and the API answers "Account already
  has an associated subdomain". The only remedies are a support request or a new
  account, so choose it as though it were permanent, because it is.

  If you leave it blank, Cloudflare derives one from your account when you first
  open the Workers dashboard -- usually from your EMAIL ADDRESS. That is how a
  personal name ends up in a public URL.
EOF
echo

SUB_API="https://api.cloudflare.com/client/v4/accounts/$ACCOUNT/workers/subdomain"
TOKEN_FILE="$HOME/Library/Preferences/.wrangler/config/default.toml"
[ -f "$TOKEN_FILE" ] || TOKEN_FILE="$HOME/.wrangler/config/default.toml"
TOKEN="$(grep '^oauth_token' "$TOKEN_FILE" 2>/dev/null | sed 's/.*= *"\(.*\)"/\1/')"

CURRENT=""
if [ -n "$TOKEN" ] && [ -n "$ACCOUNT" ]; then
    CURRENT="$(curl -s -m 15 -H "Authorization: Bearer $TOKEN" "$SUB_API" \
        | node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{try{process.stdout.write(JSON.parse(s).result?.subdomain||"")}catch{}})' 2>/dev/null)"
fi

if [ -n "$CURRENT" ]; then
    info "Already set to \"$CURRENT\" — this cannot be changed."
    if [ -n "${CURRENT##*[!a-z0-9-]*}" ] && printf '%s' "$CURRENT" | grep -q '-'; then
        warn "If that looks like your name, the only fixes are a support request"
        warn "or a fresh Cloudflare account under a project email address."
    fi
elif [ -z "$TOKEN" ]; then
    warn "Could not read a wrangler token, so this must be done in the dashboard."
else
    read -r -p "  Subdomain to claim (e.g. opendoctrines): " WANT
    if [ -n "$WANT" ]; then
        RESP="$(curl -s -m 20 -X PUT -H "Authorization: Bearer $TOKEN" \
                 -H "content-type: application/json" \
                 --data "{\"subdomain\":\"$WANT\"}" "$SUB_API")"
        if printf '%s' "$RESP" | grep -q '"success":true'; then
            info "Claimed $WANT.workers.dev"
        else
            warn "Could not claim it:"
            printf '%s\n' "$RESP" | head -c 400
            echo
            warn "It may already be taken — subdomains are global across Cloudflare."
        fi
    fi
fi

# ------------------------------------------------------------------- KV ----

bold "3. KV namespace"

# A namespace id from a DIFFERENT account is worse than none: the id is
# syntactically fine, so a naive "is it filled in?" check skips creation and the
# deploy then fails on a binding that does not exist here. Verify it is ours.
CONFIGURED="$(grep -A2 'kv_namespaces' wrangler.toml | grep -oE '[0-9a-f]{32}' | head -1 || true)"
if [ -n "$CONFIGURED" ]; then
    if ! wrangler kv namespace list 2>/dev/null | grep -q "$CONFIGURED"; then
        warn "wrangler.toml names a KV namespace this account does not have."
        warn "It probably belongs to a previous account. Creating a fresh one."
        node -e '
          const fs = require("fs");
          fs.writeFileSync("wrangler.toml", fs.readFileSync("wrangler.toml", "utf8")
            .replace(process.argv[1], "REPLACE_WITH_YOUR_KV_NAMESPACE_ID"));
        ' "$CONFIGURED"
    fi
fi

if grep -q 'REPLACE_WITH_YOUR_KV_NAMESPACE_ID' wrangler.toml; then
    info "Creating OD_ACCOUNTS…"
    OUT="$(wrangler kv namespace create OD_ACCOUNTS 2>&1)" || { echo "$OUT"; die "could not create the namespace"; }
    ID="$(printf '%s' "$OUT" | grep -oE '[0-9a-f]{32}' | head -1)"
    [ -n "$ID" ] || { echo "$OUT"; die "could not find the namespace id in wrangler's output"; }
    # In-place, with a backup, because this file is the only record of the id.
    node -e '
      const fs = require("fs");
      const f = "wrangler.toml";
      fs.copyFileSync(f, f + ".bak");
      fs.writeFileSync(f, fs.readFileSync(f, "utf8")
        .replace("REPLACE_WITH_YOUR_KV_NAMESPACE_ID", process.argv[1]));
    ' "$ID"
    info "Created and written into wrangler.toml (id ${ID:0:8}…)"
else
    info "Already configured in wrangler.toml"
fi

# ---------------------------------------------------------------- secrets ----

secret_exists() {
    wrangler secret list 2>/dev/null | grep -q "\"$1\"" || \
    wrangler secret list 2>/dev/null | grep -q "^ *$1 *$"
}

put_secret() {   # name, value on stdin
    printf '%s' "$2" | wrangler secret put "$1" >/dev/null 2>&1
}

bold "4. Signing and hashing keys"
EXISTING="$(wrangler secret list 2>/dev/null || echo '[]')"

if printf '%s' "$EXISTING" | grep -q 'IDENT_KEY'; then
    info "IDENT_KEY and PAIRWISE_KEY already set — left alone."
    warn "Rotating these would lock every existing account out permanently."
else
    info "Generating an Ed25519 keypair and two HMAC keys…"
    KEYS="$(node -e '
      const { webcrypto: c } = require("node:crypto");
      const b64 = (n) => require("node:crypto").randomBytes(n).toString("base64");
      c.subtle.generateKey("Ed25519", true, ["sign", "verify"]).then(async (k) => {
        console.log(JSON.stringify({
          priv: JSON.stringify(await c.subtle.exportKey("jwk", k.privateKey)),
          pub:  JSON.stringify(await c.subtle.exportKey("jwk", k.publicKey)),
          ident: b64(32), pairwise: b64(32), admin: b64(32),
        }));
      }).catch((e) => { console.error(e); process.exit(1); });
    ')" || die "your node build does not support Ed25519 (needs node 18+)"

    read_key() { printf '%s' "$KEYS" | node -e '
      let s = ""; process.stdin.on("data", (d) => s += d)
        .on("end", () => process.stdout.write(JSON.parse(s)[process.argv[1]]));
    ' "$1"; }

    put_secret ED25519_PRIVATE_KEY "$(read_key priv)"
    put_secret ED25519_PUBLIC_JWK  "$(read_key pub)"
    put_secret IDENT_KEY           "$(read_key ident)"
    put_secret PAIRWISE_KEY        "$(read_key pairwise)"

    ADMIN="$(read_key admin)"
    put_secret ADMIN_SECRET "$ADMIN"
    info "Done."
    echo
    bold "   Your admin secret — save it now, it is not stored anywhere else:"
    printf '     %s\n' "$ADMIN"
    echo
    info "   It grants badges:"
    info "     curl -X POST \$ISSUER/admin/badge -H \"x-od-admin: \$ADMIN_SECRET\" \\"
    info "          -d '{\"accountId\":\"…\",\"badge\":\"developer\",\"on\":true}'"
    echo
    read -r -p "   Press enter once you have saved it… " _
fi

# ------------------------------------------------------------------ oauth ----

bold "5. Sign-in providers"
cat <<'EOF'
  Register at least one. Each needs this redirect URI, with <worker> replaced by
  the URL printed at the end of this script:

      https://<worker>/auth/callback/google
      https://<worker>/auth/callback/discord
      https://<worker>/auth/callback/github

  Google   https://console.cloud.google.com/apis/credentials
           "Create credentials" > "OAuth client ID" > Web application.
           Do NOT add the email or profile scopes; the code asks for `openid`
           only, and widening it would make PRIVACY.md untrue.

  Discord  https://discord.com/developers/applications
           New Application > OAuth2 > add the redirect. Scope: identify only.

  GitHub   https://github.com/settings/developers
           New OAuth App. No scopes are requested at all.

  Leave a provider blank to skip it; the game only offers ones that are set up.
EOF
echo

for P in GOOGLE DISCORD GITHUB; do
    if printf '%s' "$EXISTING" | grep -q "${P}_CLIENT_ID"; then
        info "$P already configured — skipping."
        continue
    fi
    read -r -p "  $P client id (enter to skip): " CID
    [ -n "$CID" ] || continue
    read -r -s -p "  $P client secret: " CSEC; echo
    [ -n "$CSEC" ] || { warn "no secret given, skipping $P"; continue; }
    put_secret "${P}_CLIENT_ID" "$CID"
    put_secret "${P}_CLIENT_SECRET" "$CSEC"
    info "$P set."
done

# -------------------------------------------------------------- blocklist ----

bold "6. Nickname blocklist"
if [ -f blocklist/profanity.txt ]; then
    wrangler kv key put --binding OD_ACCOUNTS cfg:blocklist --path blocklist/profanity.txt --remote >/dev/null
    info "Uploaded blocklist/profanity.txt"
else
    warn "blocklist/profanity.txt is absent, so only the reserved-word list applies."
    info "See blocklist/README.md. You can upload it any time without redeploying:"
    info "  npx wrangler kv key put --binding OD_ACCOUNTS cfg:blocklist \\"
    info "      --path blocklist/profanity.txt --remote"
fi

# ----------------------------------------------------------------- deploy ----

bold "7. Deploy"
if grep -q 'CONTROLLER NAME' PRIVACY.md; then
    warn "PRIVACY.md still has [CONTROLLER NAME] and [CONTACT EMAIL] in it."
    warn "Those are required under the GDPR and are served at /privacy as-is."
    read -r -p "  Deploy anyway? [y/N] " GO
    [ "$GO" = "y" ] || die "fill them in and re-run"
fi

wrangler deploy

URL="$(wrangler deployments list 2>/dev/null | grep -oE 'https://[a-z0-9.-]+workers\.dev' | head -1 || true)"
if [ -z "$URL" ]; then
    NAME="$(grep -E '^name *=' wrangler.toml | head -1 | sed 's/.*"\(.*\)".*/\1/')"
    URL="https://$NAME.<your-subdomain>.workers.dev"
fi

echo
bold "Almost done — one manual step"
cat <<EOF

  ISSUER in wrangler.toml must match the deployed URL exactly. It is stamped
  into every token as \`iss\`, and game servers check it to decide whether a
  badge is official, so a mismatch means nothing verifies.

    1. Set  ISSUER = "$URL"  in wrangler.toml
    2. npx wrangler deploy
    3. Add the redirect URIs above to each provider, using that same host.

  Then check it is alive:

    curl $URL/
    curl $URL/.well-known/od-keys.json

EOF
