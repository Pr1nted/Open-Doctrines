#!/usr/bin/env bash
# Keep a dedicated server's campaign in a private git repository, so the
# machine running it can be switched off, wiped or replaced.
#
#   tools/campaign_sync.sh init   <repo-url>   first time: push this machine's campaign
#   tools/campaign_sync.sh pull                restore the campaign before the server starts
#   tools/campaign_sync.sh push                save it (the server runs this itself)
#   tools/campaign_sync.sh heartbeat           say "this machine is hosting", cheaply
#   tools/campaign_sync.sh release             say "this machine has stopped"
#
# WHY GIT
#
# A free hosting platform gives a server a disk that is wiped whenever it
# restarts, and a free account at any git host gives a private repository that
# is not. The server calls `push` after every turn and within a minute of orders
# arriving (ServerConfig::checkpointCommand), and `pull` puts it all back before
# the next start. History is NOT kept: each push replaces the branch with one
# commit, so the repository stays the size of one campaign rather than growing
# by a save every day. The commit before it is kept as `previous`, so a bad
# push can be undone by hand.
#
# WHAT IS IN IT -- AND WHY THE REPOSITORY MUST BE PRIVATE
#
#   saves/<world>.odsv   the world, every turn
#   .odhost              seats, bans, the open turn's deadline, the invite code
#   .odorders            orders already in for the open turn
#   .odkey               the key long-form orders are sealed with
#   server.json          the server's settings
#   config.json          ONLY the account service and the server credential --
#                        the credential is what keeps every player's identity
#                        the same on this server; lose it and every seat is a
#                        stranger's
#   account.json         the session token the server hosts with
#
# The token and the credential together can open sessions as you. They cannot
# join games as you, read your account, or reach anything but this server. A
# public repository would publish both: the script refuses a github.com URL that
# answers without credentials, and you should still check.
#
# TWO MACHINES, ONE CAMPAIGN
#
# The same campaign running twice would be two worlds diverging, each believing
# it is the real one. `pull` refuses to start while another machine's heartbeat
# is less than 15 minutes old (OD_SYNC_FORCE=1 overrides, for a machine that
# died without saying so). The heartbeat lives on its own tiny branch so that
# beating it every few minutes does not re-upload the world.
#
# Environment:
#   OD_CAMPAIGN_REPO     git URL with credentials, e.g.
#                        https://x-access-token:<token>@github.com/you/od-campaign.git
#   OD_DATA_DIR          the server's data directory (saves/, config.json, account.json)
#   OD_SERVER_CONFIG     server.json (default: $OD_DATA_DIR/../server.json)
#   OD_SYNC_HOST         a name for this machine in the heartbeat (default: hostname)
#   OD_SYNC_FORCE=1      start even though another machine looks alive
set -euo pipefail

cmd="${1:-}"
[ -n "$cmd" ] || { sed -n '2,12p' "$0"; exit 2; }
[ "$cmd" = init ] && export OD_CAMPAIGN_REPO="${2:-${OD_CAMPAIGN_REPO:-}}"

repo="${OD_CAMPAIGN_REPO:-}"
[ -n "$repo" ] || { echo "campaign_sync: set OD_CAMPAIGN_REPO" >&2; exit 2; }
data="${OD_DATA_DIR:-}"
[ -n "$data" ] || { echo "campaign_sync: set OD_DATA_DIR" >&2; exit 2; }
data="${data%/}"
cfg="${OD_SERVER_CONFIG:-$data/../server.json}"
me="${OD_SYNC_HOST:-$(hostname 2>/dev/null || echo unknown)}"
work="${OD_SYNC_WORK:-${TMPDIR:-/tmp}/od-campaign-sync}"
branch=campaign
lockbranch=heartbeat

say() { echo "campaign_sync: $*"; }
redact() { sed -E 's#(https?://)[^@/]*@#\1***@#g'; }
git_q() { git -c user.name=od-server -c user.email=od-server@localhost "$@"; }

# A public repository would publish the token. github.com answers an anonymous
# request for a private repository with 404, so a 200 means anyone can read it.
check_private() {
    case "$repo" in
        https://*github.com/*)
            local bare; bare="$(printf '%s' "$repo" | sed -E 's#https://[^@/]*@#https://#; s#\.git$##')"
            local api; api="$(printf '%s' "$bare" | sed -E 's#https://github.com/#https://api.github.com/repos/#')"
            if command -v curl >/dev/null &&
               [ "$(curl -s -o /dev/null -w '%{http_code}' "$api")" = 200 ]; then
                say "REFUSING: $bare is PUBLIC. It would publish your session token and"
                say "server credential. Make the repository private and try again."
                exit 4
            fi ;;
    esac
}

checkout() {                       # a fresh shallow copy of <branch> in $work/<branch>
    local b="$1" dir="$work/$1"
    rm -rf "$dir"
    if git ls-remote --exit-code --heads "$repo" "$b" >/dev/null 2>&1; then
        git clone -q --depth 1 --branch "$b" "$repo" "$dir" 2>&1 | redact
    else
        mkdir -p "$dir"
        ( cd "$dir" && git init -q && git checkout -q -b "$b" && git remote add origin "$repo" )
    fi
}

world_files() {                    # the files of the world server.json names
    local save; save="$(sed -n 's/.*"load-save": "\([^"]*\)".*/\1/p' "$cfg" 2>/dev/null | head -1)"
    [ -n "$save" ] || return 0
    for f in "$save" "$save.odhost" "$save.odorders" "$save.odkey"; do
        [ -f "$data/saves/$f" ] && echo "saves/$f"
    done
    return 0
}

beat() {                           # write the heartbeat; empty $1 means "released"
    checkout "$lockbranch"
    printf '%s %s\n' "${1:-}" "$(date +%s)" > "$work/$lockbranch/lock"
    ( cd "$work/$lockbranch" && git_q add lock && git_q commit -q -m "heartbeat" --allow-empty &&
      git push -q -f origin "HEAD:$lockbranch" 2>&1 | redact )
}

case "$cmd" in
init|push)
    [ "$cmd" = init ] && check_private
    [ -f "$cfg" ] || { say "no server config at $cfg"; exit 2; }
    checkout "$branch"
    dir="$work/$branch"
    # Keep the last push one step back, as `previous`.
    if [ -d "$dir/saves" ]; then
        rm -rf "$dir/previous" && mkdir -p "$dir/previous"
        cp -R "$dir/saves" "$dir/previous/" 2>/dev/null || true
    fi
    rm -rf "$dir/saves" && mkdir -p "$dir/saves"
    for f in $(world_files); do cp "$data/$f" "$dir/$f"; done
    cp "$cfg" "$dir/server.json"
    [ -f "$data/account.json" ] && cp "$data/account.json" "$dir/account.json"
    # Only the two keys a server needs from the game's config, not the player's
    # settings around them.
    if [ -f "$data/config.json" ]; then
        python3 - "$data/config.json" "$dir/config.json" <<'PY' || { say "python3 is needed"; exit 2; }
import json, sys
src = json.load(open(sys.argv[1]))
keep = {k: src[k] for k in ("accountIssuer", "serverCredential") if k in src}
json.dump(keep, open(sys.argv[2], "w"), indent=2)
PY
    fi
    cat > "$dir/README.md" <<'EOF'
# OpenDoctrines campaign

Written by `tools/campaign_sync.sh`. Keep this repository PRIVATE: it holds the
server's session token and credential. `previous/` is the push before this one.
EOF
    ( cd "$dir" &&
      git_q checkout -q --orphan "next-$$" &&
      git_q add -A &&
      git_q commit -q -m "campaign checkpoint $(date -u +%Y-%m-%dT%H:%M:%SZ) from $me" &&
      git push -q -f origin "HEAD:$branch" 2>&1 | redact )
    beat "$me"
    say "pushed $(world_files | wc -l | tr -d ' ') world file(s)"
    ;;
pull)
    if [ "${OD_SYNC_FORCE:-0}" != 1 ]; then
        checkout "$lockbranch"
        if [ -f "$work/$lockbranch/lock" ]; then
            read -r who when < "$work/$lockbranch/lock" || true
            age=$(( $(date +%s) - ${when:-0} ))
            if [ -n "${who:-}" ] && [ "$who" != "$me" ] && [ "$age" -lt 900 ]; then
                say "REFUSING: $who was hosting this campaign ${age}s ago."
                say "Stop it first, or set OD_SYNC_FORCE=1 if it is gone for good."
                exit 3
            fi
        fi
    fi
    checkout "$branch"
    dir="$work/$branch"
    if [ ! -f "$dir/server.json" ]; then
        say "the repository has no campaign yet; starting fresh"
        exit 0
    fi
    mkdir -p "$data/saves"
    cp "$dir/server.json" "$cfg"
    [ -d "$dir/saves" ] && cp -R "$dir/saves/." "$data/saves/"
    [ -f "$dir/account.json" ] && { cp "$dir/account.json" "$data/account.json"; chmod 600 "$data/account.json"; }
    if [ -f "$dir/config.json" ]; then
        if [ -f "$data/config.json" ]; then
            # Merge the two keys into whatever config is there.
            python3 - "$data/config.json" "$dir/config.json" <<'PY' 2>/dev/null || cp "$dir/config.json" "$data/config.json"
import json, sys
base = json.load(open(sys.argv[1]))
base.update(json.load(open(sys.argv[2])))
json.dump(base, open(sys.argv[1], "w"), indent=2)
PY
        else
            cp "$dir/config.json" "$data/config.json"
        fi
    fi
    beat "$me"
    say "restored $(ls "$dir/saves" 2>/dev/null | wc -l | tr -d ' ') world file(s)"
    ;;
heartbeat) beat "$me" ;;
release)   beat "" ; say "released" ;;
*) echo "campaign_sync: unknown command $cmd" >&2; exit 2 ;;
esac
