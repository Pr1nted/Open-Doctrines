#!/usr/bin/env bash
#
# Point the in-game report form at somewhere you will actually read.
#
#   ./add-feedback.sh
#
# Sets the four destination settings, skipping any you leave blank, and then
# CHECKS THAT THE ENDPOINT IS ACTUALLY LIVE -- because `wrangler secret put`
# succeeds whether or not the Worker it is talking to has the route, and the
# first version of this feature was configured against a deployment that did
# not have /feedback at all. A secret on a route that 404s is invisible until
# a player writes a report and it vanishes.
#
# Secrets are read with `read -s` and piped, so they never reach your shell
# history nor a command line that `ps` could show. Same as add-provider.sh.
#
# Nothing here is required. With none of it set the endpoint still validates
# and rate-limits and simply forwards nowhere, which is the correct state for
# a fork that has not set up a tracker of its own.

set -euo pipefail
cd "$(dirname "$0")"

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
info() { printf '  %s\n' "$1"; }
warn() { printf '\033[33m  %s\033[0m\n' "$1"; }
die()  { printf '\033[31mError: %s\033[0m\n' "$1" >&2; exit 1; }

wrangler() { npx --no-install wrangler "$@"; }
trim() { printf '%s' "$1" | tr -d '[:space:]'; }

ISSUER="$(grep -E '^ISSUER' wrangler.toml | sed 's/.*= *"\(.*\)"/\1/')"

# The repository this checkout points at.
#
# Secrets are write-only -- wrangler can list their names and never their values
# -- so a re-run cannot ask the Worker which repo is configured. Without this,
# leaving prompt 3 blank (because it is already set, which is the whole point of
# "blank keeps it") left every later step that needs a repo name silently
# skipped. The git remote knows, and it is right here.
REPO_GUESS=""
if command -v git >/dev/null 2>&1; then
    REPO_GUESS="$(git config --get remote.origin.url 2>/dev/null || true)"
    REPO_GUESS="${REPO_GUESS#git@github.com:}"
    REPO_GUESS="${REPO_GUESS#https://github.com/}"
    REPO_GUESS="${REPO_GUESS#http://github.com/}"
    REPO_GUESS="${REPO_GUESS%.git}"
    case "$REPO_GUESS" in */*/*|"") REPO_GUESS="" ;; */*) ;; *) REPO_GUESS="" ;; esac
fi
[ -n "$ISSUER" ] || die "no ISSUER in wrangler.toml"

bold "Feedback destinations for $ISSUER"
echo

# ── The route has to exist before a secret on it means anything ──
printf '  Checking /feedback is deployed... '
CODE="$(curl -s -m 15 -o /dev/null -w '%{http_code}' -X POST \
        -H 'content-type: application/json' -d '{}' "$ISSUER/feedback" || echo 000)"
case "$CODE" in
    400|429) printf 'yes\n\n' ;;   # it parsed our empty body and refused it: the route is live
    404) echo "no"
         die "this deployment has no /feedback route. Run 'npx wrangler deploy' first." ;;
    000) echo "unreachable"
         die "could not reach $ISSUER" ;;
    *)   printf 'unexpected HTTP %s\n\n' "$CODE"
         warn "continuing, but check the route by hand" ;;
esac

info "Anything you leave blank is skipped, and keeps whatever is already set."
echo

# ── Discord: where bugs and suggestions land ──
#
# Forum channels work and are the better choice: each report becomes its own
# post with its own comment thread. The sender adds a thread_name when the
# channel needs one and falls back to a plain message when it does not, so
# either kind of channel is fine here and nothing has to be declared.
# Which channel a webhook actually posts to.
#
# A webhook URL contains its own credential, so this needs nothing else -- and
# it turns an opaque 121-character paste into a name you can recognise. Worth
# the request: the failure it catches is pasting the same URL into two prompts,
# which is silent, easy, and only discovered when the wrong channel fills up.
LAST_CHANNEL=""
describe_hook() {   # $1 = webhook url; sets LAST_CHANNEL
    LAST_CHANNEL=""
    command -v python3 >/dev/null 2>&1 || return 0
    local body
    body="$(curl -s -m 10 "$1" 2>/dev/null)" || return 0
    LAST_CHANNEL="$(printf '%s' "$body" | python3 -c '
import json,sys
try:
    d = json.load(sys.stdin)
except Exception:
    raise SystemExit
print(d.get("channel_id",""))
' 2>/dev/null)"
    local name
    name="$(printf '%s' "$body" | python3 -c '
import json,sys
try:
    d = json.load(sys.stdin)
except Exception:
    raise SystemExit
print(d.get("name",""))
' 2>/dev/null)"
    [ -n "$LAST_CHANNEL" ] && info "  -> posts as \"$name\" into channel $LAST_CHANNEL"
    return 0
}

BUG_CHANNEL=""
set_hook() {   # $1 = secret name
    local url
    read -r -s -p "  $1 (blank to skip): " url; echo
    url="$(trim "$url")"
    if [ -z "$url" ]; then info "Skipped."; return; fi
    case "$url" in
        https://discord.com/api/webhooks/*|https://discordapp.com/api/webhooks/*) ;;
        *) die "that does not look like a Discord webhook URL" ;;
    esac
    describe_hook "$url"

    # The mistake this exists to catch. Two secrets pointing at one channel is
    # indistinguishable, afterwards, from the routing being broken.
    if [ -n "$LAST_CHANNEL" ]; then
        case "$1" in
            FEEDBACK_DISCORD_WEBHOOK) BUG_CHANNEL="$LAST_CHANNEL" ;;
            *) if [ -n "$BUG_CHANNEL" ] && [ "$LAST_CHANNEL" = "$BUG_CHANNEL" ]; then
                   warn "  That is the SAME channel as the bug webhook."
                   warn "  Everything would land in one place. Leave this blank instead,"
                   warn "  or paste the webhook for the other channel."
                   read -r -p "  Set it anyway? [y/N]: " yn
                   case "$yn" in y|Y|yes|YES) ;; *) info "Skipped."; return ;; esac
               fi ;;
        esac
    fi

    printf '%s' "$url" | wrangler secret put "$1" >/dev/null 2>&1 \
        || die "could not set $1"
    info "Set (${#url} chars)."
}

bold "1. Discord channel for bug reports"
info "Channel > Edit Channel > Integrations > Webhooks > New Webhook > Copy URL."
info "A forum channel is fine, and is the better home: one post per report."
set_hook FEEDBACK_DISCORD_WEBHOOK
echo

bold "2. Discord channel for suggestions (optional)"
info "A separate channel, if you have one. An idea is a conversation and a bug"
info "is a job; mixing them makes the bug list worse at being a bug list."
info "Blank means suggestions go to the bug channel above."
set_hook FEEDBACK_DISCORD_SUGGESTION_WEBHOOK
echo

# ── GitHub: the public tracker, and the PRIVATE advisory ──
bold "3. GitHub repository"
info "Ordinary reports become public issues here."
info "SECURITY reports become a private DRAFT ADVISORY here instead, never an issue."
if [ -n "$REPO_GUESS" ]; then
    info "This checkout points at $REPO_GUESS."
fi
read -r -p "  FEEDBACK_GITHUB_REPO, owner/repo or the repo URL (blank to skip): " REPO
REPO="$(trim "$REPO")"
if [ -n "$REPO" ]; then
    # Take the URL too. Copying the address out of the browser is the obvious
    # thing to do here, and refusing it teaches nothing -- owner/repo is what
    # the API wants, not what a person has to hand.
    REPO="${REPO#https://}"; REPO="${REPO#http://}"
    REPO="${REPO#www.}"; REPO="${REPO#github.com/}"
    REPO="${REPO%.git}"; REPO="${REPO%/}"
    case "$REPO" in
        */*/*|/*|*/) die "expected owner/repo, e.g. Pr1nted/Open-Doctrines" ;;
        */*) ;;
        *) die "expected owner/repo, e.g. Pr1nted/Open-Doctrines" ;;
    esac
    info "Repository: $REPO"
    echo
    bold "4. GitHub token"
    info "A fine-grained token on $REPO needs BOTH of these, or half of this breaks:"
    info "    Issues:                        Read and write"
    info "    Repository security advisories: Read and write"
    info "A classic token needs the 'repo' scope."
    info "WITHOUT the advisory permission, security reports cannot be filed at all --"
    info "they are never re-routed to a public issue to make delivery succeed."
    read -r -s -p "  FEEDBACK_GITHUB_TOKEN: " TOK; echo
    TOK="$(trim "$TOK")"
    [ -n "$TOK" ] || die "a repo was given but no token; neither works without the other"

    printf '%s' "$REPO" | wrangler secret put FEEDBACK_GITHUB_REPO  >/dev/null 2>&1 \
        || die "could not set FEEDBACK_GITHUB_REPO"
    printf '%s' "$TOK"  | wrangler secret put FEEDBACK_GITHUB_TOKEN >/dev/null 2>&1 \
        || die "could not set FEEDBACK_GITHUB_TOKEN"
    info "Set (token ${#TOK} chars)."

    # Checked HERE rather than left to the first real security report, which is
    # the worst possible moment to discover the token is missing a permission.
    # Tests WRITE, and creates nothing.
    #
    # The obvious check is a GET, and it is the wrong one: a read-only token
    # passes it and then cannot file a thing. This POSTs a deliberately empty
    # advisory instead. GitHub checks authorisation before it validates the
    # body, so the two answers separate cleanly:
    #
    #   422  allowed -- it got as far as objecting to the empty body
    #   403  not allowed -- it never got that far
    #
    # Nothing is created either way, because the body never validates.
    echo
    printf '  Checking the token can CREATE a security advisory... '
    ADV="$(curl -s -m 15 -o /dev/null -w '%{http_code}' -X POST \
           -H "authorization: Bearer $TOK" \
           -H "accept: application/vnd.github+json" \
           -H "content-type: application/json" \
           -H "user-agent: opendoctrines-feedback" \
           -d '{}' \
           "https://api.github.com/repos/$REPO/security-advisories" || echo 000)"
    case "$ADV" in
        422) echo "yes" ;;
        403|404) echo "NO"
                 warn "The token cannot create a security advisory on $REPO."
                 warn "Set 'Repository security advisories' to READ AND WRITE at"
                 warn "  https://github.com/settings/personal-access-tokens"
                 warn "then re-run this. Read-only is not enough and looks fine."
                 warn "Everything else is set; only security reports are affected." ;;
        401) echo "NO"; die "the token was rejected outright (401)" ;;
        201) echo "yes"
             warn "An empty advisory was somehow accepted -- check $REPO for a stray draft." ;;
        *)   printf 'unclear (HTTP %s)\n' "$ADV" ;;
    esac
else
    info "Skipped."
fi
echo

# ── Optional: a separate channel for security, if the advisory ever fails ──
bold "5. Optional fallback channel for security reports"
info "Used ONLY if the advisory cannot be filed. Leave blank unless you have a"
info "channel that is genuinely more private than your bugs channel -- the point"
info "of this path is that an unfixed exploit is not readable by the whole server."
info "NOTE: security reports do NOT go to your bugs channel, forum tag or not."
set_hook FEEDBACK_DISCORD_SECURITY_WEBHOOK
echo

# ── Closing an issue tells the thread it came from ──
#
# GitHub pushes issue events here; the service posts "Closed by X" into the
# Discord thread the report opened. One direction only: Discord cannot push
# events to a URL, so the reverse would need a bot on a gateway connection.
HOOK_REPO="${REPO:-$REPO_GUESS}"
if [ -n "$HOOK_REPO" ]; then
    bold "6. Tell the Discord thread when its issue is closed (optional)"
    info "Repository: $HOOK_REPO"
    info "GitHub sends issue events to the service, which posts a note in the"
    info "thread the report came from. Needs a shared secret in both places."
    read -r -p "  Set this up? [Y/n]: " yn
    case "$yn" in
        n|N|no|NO) info "Skipped." ;;
        *)
            # Generated here rather than asked for: it is a shared secret with
            # no meaning outside these two systems, and a person inventing one
            # picks a worse one than /dev/urandom does.
            # NOT `tr </dev/urandom | head -c 48`.
            #
            # That is the usual idiom and it is fatal under `set -euo pipefail`:
            # head exits the moment it has 48 bytes, tr dies of SIGPIPE with
            # status 141, pipefail promotes that to the pipeline's status, and
            # set -e kills the script -- with no message, because set -e does
            # not print one. It cost a bug report that read "I did step 6" with
            # nothing to show for it.
            #
            # Here nothing closes early: head reads a FILE and stops, tr
            # consumes all of it, cut consumes all of that. 512 bytes yields
            # ~120 alphanumerics, comfortably more than the 48 taken.
            HOOK_SECRET="$(LC_ALL=C head -c 512 /dev/urandom \
                           | LC_ALL=C tr -dc 'a-zA-Z0-9' | cut -c1-48)"
            printf '%s' "$HOOK_SECRET" | wrangler secret put FEEDBACK_GITHUB_WEBHOOK_SECRET \
                >/dev/null 2>&1 || die "could not set FEEDBACK_GITHUB_WEBHOOK_SECRET"
            info "Secret generated and stored in the Worker."

            HOOK_URL="$ISSUER/feedback/github"
            if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
                # UPDATE if one of ours is already there, rather than adding a
                # second. This script is meant to be re-run -- that is how you
                # rotate the secret -- and a repository quietly accumulating a
                # hook per run would post every closure that many times.
                # Matched on our URL, so third-party hooks are left alone.
                # `first` inside jq rather than `| head -1` outside it, for
                # the same reason as above: head closing early would SIGPIPE gh.
                EXISTING="$(gh api "repos/$HOOK_REPO/hooks" \
                    --jq "[.[] | select(.config.url == \"$HOOK_URL\") | .id] | first // empty" \
                    2>/dev/null || true)"
                if [ -n "$EXISTING" ]; then
                    printf '  Updating the existing webhook on %s... ' "$HOOK_REPO"
                    HOOK_METHOD=PATCH
                    HOOK_PATH="repos/$HOOK_REPO/hooks/$EXISTING"
                else
                    printf '  Creating the webhook on %s... ' "$HOOK_REPO"
                    HOOK_METHOD=POST
                    HOOK_PATH="repos/$HOOK_REPO/hooks"
                fi
                if gh api "$HOOK_PATH" -X "$HOOK_METHOD" \
                        -f "name=web" -F "active=true" -f "events[]=issues" \
                        -f "config[url]=$HOOK_URL" \
                        -f "config[content_type]=json" \
                        -f "config[secret]=$HOOK_SECRET" >/dev/null 2>&1; then
                    echo "done"
                    info "Close an issue that came from a report and the thread will say so."
                else
                    echo "could not"
                    warn "Add it by hand at https://github.com/$HOOK_REPO/settings/hooks:"
                    warn "  Payload URL:  $HOOK_URL"
                    warn "  Content type: application/json"
                    warn "  Secret:       $HOOK_SECRET"
                    warn "  Events:       Issues only"
                fi
            else
                info "gh is not available, so add the webhook by hand at"
                info "  https://github.com/$HOOK_REPO/settings/hooks"
                info "  Payload URL:  $HOOK_URL"
                info "  Content type: application/json"
                info "  Secret:       $HOOK_SECRET"
                info "  Events:       Issues only"
            fi
            ;;
    esac
    echo
fi

# ── Where bans and timeouts are announced ──
bold "7. Discord channel for moderation notices (optional)"
info "When you ban or time somebody out, the service posts it here."
info "It says WHO and UNTIL WHEN and nothing else -- never the reported"
info "message and never who reported it, because a moderation channel is"
info "still a room full of people. A dismissal is never announced at all."
set_hook MODERATION_DISCORD_WEBHOOK
echo

bold "Done."
info "Secrets apply without a redeploy, but the edge takes a few seconds."
info "Test it from the game: Escape > Report a problem."
info "A build only offers the form if it was configured with"
info "  -DOD_ACCOUNT_ISSUER=$ISSUER"
