#!/usr/bin/env bash
# Start a dedicated server inside the container, picking up the campaign where
# the last machine left it.
#
# Everything is configured by environment, because that is the one interface
# every hosting platform gives you:
#
#   OD_CAMPAIGN_REPO   private git repo holding the campaign (see campaign_sync.sh).
#                      Without it nothing survives a restart -- fine for a test,
#                      not for a tournament.
#   OD_ISSUER          account service (default: the official one built in)
#   OD_ACCOUNT_TOKEN   a session token, for a first start with no campaign repo yet
#   OD_SERVER_CREDENTIAL  the server's credential, likewise
#   OD_MAP             map for a NEW campaign (default 1914)
#   OD_SERVER_NAME     what the server calls itself
#   OD_TURN_SECONDS    turn length (default 86400: a turn a day)
#   OD_TURN_AT         resolve at this UTC time of day, e.g. 18:00
#   OD_VOICE_LINK      a Discord (or any https) invite offered to players
#   OD_MAX_PLAYERS     seats (default 16)
#   OD_RELAY=1         host through the account service's relay instead of $PORT
#   PORT               the port the platform routes to (Render sets it)
set -euo pipefail

state="${OD_STATE:-/state}"
data="$state/data"
cfg="$state/server.json"
bin=/opt/od/bin/OpenDoctrinesServer
mkdir -p "$data/saves"

# The read-only content, by symlink: the server writes only saves/, config.json
# and account.json, and those are the only things that need to be ours.
for e in /opt/od/data/*; do
    b="$(basename "$e")"
    case "$b" in saves|config.json|account.json) continue ;; esac
    [ -e "$data/$b" ] || ln -s "$e" "$data/$b"
done

export OD_DATA_DIR="$data" OD_SERVER_CONFIG="$cfg"
export OD_SYNC_HOST="${OD_SYNC_HOST:-${RENDER_SERVICE_NAME:-container}-$(hostname)}"

# ── what the last machine left ──
if [ -n "${OD_CAMPAIGN_REPO:-}" ]; then
    /opt/od/campaign_sync.sh pull || {
        rc=$?
        # 3: another machine is hosting. Waiting beats two diverging worlds.
        if [ "$rc" = 3 ]; then
            echo "entrypoint: another machine holds this campaign; retrying in 2 minutes"
            sleep 120
            exec "$0"
        fi
        exit "$rc"
    }
fi

# ── who this server is, the first time ──
# A token is only ever presented to the issuer that minted it, matched exactly,
# so account.json has to name the one this server will actually use.
issuer="${OD_ISSUER:-$("$bin" --print-issuer)}"
if [ ! -f "$data/account.json" ] && [ -n "${OD_ACCOUNT_TOKEN:-}" ]; then
    printf '{"issuer": "%s", "token": "%s"}\n' "$issuer" "$OD_ACCOUNT_TOKEN" > "$data/account.json"
    chmod 600 "$data/account.json"
fi
if [ ! -f "$data/config.json" ]; then
    python3 - "$data/config.json" <<'PY'
import json, os, sys
c = {"accountAgreed": True}
if os.environ.get("OD_ISSUER"): c["accountIssuer"] = os.environ["OD_ISSUER"]
if os.environ.get("OD_SERVER_CREDENTIAL"): c["serverCredential"] = os.environ["OD_SERVER_CREDENTIAL"]
json.dump(c, open(sys.argv[1], "w"), indent=2)
PY
fi

# ── the server's settings: written once, then the campaign's own ──
if [ ! -f "$cfg" ]; then
    "$bin" --write-config --config "$cfg" --data "$data" >/dev/null
fi
python3 - "$cfg" <<'PY'
import os, re, sys
p = sys.argv[1]
s = open(p).read()
def put(key, value):
    global s
    s = re.sub(r'"%s": [^,\n]+' % re.escape(key), '"%s": %s' % (key, value), s)
def text(v): return '"' + v.replace('\\', '\\\\').replace('"', '\\"') + '"'
env = os.environ
# Where the platform routes traffic, every start: it can change between starts.
relay = env.get("OD_RELAY") == "1"
put("port", env.get("PORT", "27015"))
put("bind-all", "false" if relay else "true")
put("relay", "true" if relay else "false")
put("tunnel", '"off"')
put("checkpoint-command", text("/opt/od/campaign_sync.sh push" if env.get("OD_CAMPAIGN_REPO") else ""))
# A new campaign's shape. Once a world exists (load-save is set) these are the
# campaign's and the environment no longer overrides them, except where noted.
fresh = '"load-save": ""' in s
if fresh:
    put("map", text(env.get("OD_MAP", "1914")))
    put("turn-seconds", env.get("OD_TURN_SECONDS", "86400"))
    put("max-players", env.get("OD_MAX_PLAYERS", "16"))
    put("auto.start-at-players", env.get("OD_START_AT_PLAYERS", "0"))
    put("late-join", '"spectate"')
if env.get("OD_SERVER_NAME"): put("name", text(env["OD_SERVER_NAME"]))
if "OD_TURN_AT" in env: put("turn-at", text(env["OD_TURN_AT"]))
if "OD_VOICE_LINK" in env: put("voice-link", text(env["OD_VOICE_LINK"]))
open(p, "w").write(s)
PY

# ── the heartbeat that keeps a second machine from starting this campaign ──
if [ -n "${OD_CAMPAIGN_REPO:-}" ]; then
    ( while sleep 300; do /opt/od/campaign_sync.sh heartbeat >/dev/null 2>&1 || true; done ) &
    beat_pid=$!
fi

# ── run ──
# Not exec: when the server stops, the heartbeat has to say so, or the next
# machine waits fifteen minutes for a host that is already gone.
set +e
"$bin" --config "$cfg" --data "$data" < /dev/null &
srv=$!
trap 'kill -TERM "$srv" 2>/dev/null' TERM INT
wait "$srv"; rc=$?
# A SIGTERM interrupts the first wait; wait again for the save and push.
kill -0 "$srv" 2>/dev/null && { wait "$srv"; rc=$?; }
if [ -n "${OD_CAMPAIGN_REPO:-}" ]; then
    kill "${beat_pid:-0}" 2>/dev/null
    /opt/od/campaign_sync.sh release || true
fi
exit "$rc"
