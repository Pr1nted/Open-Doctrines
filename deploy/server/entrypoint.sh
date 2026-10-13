#!/usr/bin/env bash
# Start a dedicated server inside the container, picking up the campaign where
# the last instance left it.
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
#   OD_MAP             map for a NEW campaign (default 1914; `map` is Modern Day)
#   OD_SERVER_NAME     what the server calls itself
#   OD_TURN_SECONDS    turn length for a NEW campaign (default 86400: a turn a day)
#   OD_TURN_AT         resolve at this UTC time of day, e.g. 18:00
#   OD_VOICE_LINK      a Discord (or any https) invite offered to players
#   OD_MAX_PLAYERS     seats (default 16)
#   OD_START_AT_PLAYERS  start by itself once this many hold a country; 0 (the
#                      default) keeps it a lobby until the host says `start`
#   OD_RELAY=1         host through the account service's relay: players join
#                      with the code alone. Otherwise the server listens on $PORT.
#   PORT               the port the platform routes to (Render sets it)
#   OD_LLM_ENDPOINT    an OpenAI-style chat API for the countries nobody plays,
#                      e.g. https://api.groq.com/openai/v1; unset: no AI letters
#   OD_LLM_MODEL       the model there, e.g. openai/gpt-oss-20b
#   OD_LLM_FALLBACK_MODELS  more models, comma-separated, asked in order when
#                      one has used its daily allowance (each has its own)
#   OD_LLM_KEY         its API key. Written into data/config.json on every start
#                      and nowhere else: campaign_sync.sh copies only the account
#                      issuer and the server credential out of that file, so the
#                      key cannot reach the campaign repository.
#
# ── HOW AN UPDATE HANDS OVER ──
#
# Render starts the new instance first and stops the old one once the new one
# is healthy. Two servers must never host one campaign, so the new instance
# waits for the old one's lock (campaign_sync.sh) -- and it must look healthy
# WHILE it waits, or Render would never stop the old one and the two would wait
# for each other until the deploy timed out. So with OD_RELAY=1 the platform's
# port belongs to deploy/server/health.py from the first second, and players,
# who come by the relay, never need it. The old instance, on SIGTERM, saves,
# pushes and releases the lock; the new one then pulls and starts.
set -euo pipefail

state="${OD_STATE:-/state}"
data="$state/data"
cfg="$state/server.json"
bin=/opt/od/bin/OpenDoctrinesServer
status="$state/status"
mkdir -p "$data/saves"
stage() { printf '%s\n' "$*" > "$status"; echo "entrypoint: $*"; }

relay=0
[ "${OD_RELAY:-}" = 1 ] && relay=1

# The health responder, first: see above.
if [ "$relay" = 1 ]; then
    stage "starting"
    python3 /opt/od/health.py "${PORT:-10000}" "$status" &
    health_pid=$!
fi

# The read-only content, by symlink: the server writes only saves/, config.json
# and account.json, and those are the only things that need to be ours.
for e in /opt/od/data/*; do
    b="$(basename "$e")"
    case "$b" in saves|config.json|account.json) continue ;; esac
    [ -e "$data/$b" ] || ln -s "$e" "$data/$b"
done

export OD_DATA_DIR="$data" OD_SERVER_CONFIG="$cfg"
# One name per INSTANCE, so an instance can tell its predecessor's lock from
# its own: a redeploy runs both for a while.
export OD_SYNC_HOST="${OD_SYNC_HOST:-${RENDER_SERVICE_NAME:-container}-${RENDER_INSTANCE_ID:-$(hostname)}}"

# ── what the last instance left ──
if [ -n "${OD_CAMPAIGN_REPO:-}" ]; then
    waited=0
    while :; do
        rc=0
        /opt/od/campaign_sync.sh pull || rc=$?
        [ "$rc" = 0 ] && break
        # 3: another instance holds the campaign -- the one this is replacing,
        # which lets go as soon as it is told to stop, or a crashed one, whose
        # lock goes stale. Anything else is a real failure.
        [ "$rc" = 3 ] || { stage "could not restore the campaign (campaign_sync $rc)"; exit "$rc"; }
        stage "waiting for the previous instance to hand over (${waited}s)"
        sleep 10
        waited=$((waited + 10))
    done
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

# ── the language model, from the environment, every start ──
python3 - "$data/config.json" <<'PY'
import json, os, sys
p = sys.argv[1]
c = json.load(open(p)) if os.path.exists(p) else {}
e = os.environ
if e.get("OD_LLM_ENDPOINT") and e.get("OD_LLM_MODEL"):
    c.update({"llmEnabled": True, "llmEndpoint": e["OD_LLM_ENDPOINT"],
              "llmModel": e["OD_LLM_MODEL"], "llmApiKey": e.get("OD_LLM_KEY", ""),
              "llmFallbackModels": e.get("OD_LLM_FALLBACK_MODELS", "")})
else:
    c["llmEnabled"] = False
json.dump(c, open(p, "w"), indent=2)
os.chmod(p, 0o600)
PY

# ── the server's settings: written once, then the campaign's own ──
if [ ! -f "$cfg" ]; then
    "$bin" --write-config --config "$cfg" --data "$data" >/dev/null
fi
python3 - "$cfg" "$relay" <<'PY'
import os, re, sys
p, relay = sys.argv[1], sys.argv[2] == "1"
s = open(p).read()
def put(key, value):
    global s
    s = re.sub(r'"%s": [^,\n]+' % re.escape(key), '"%s": %s' % (key, value), s)
def text(v): return '"' + v.replace('\\', '\\\\').replace('"', '\\"') + '"'
env = os.environ
# Where the platform routes traffic, every start: it can change between starts.
# Relayed, the server listens on nothing; $PORT is health.py's.
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
    put("auto.return-to-lobby", "false")
    put("late-join", '"spectate"')
if env.get("OD_SERVER_NAME"): put("name", text(env["OD_SERVER_NAME"]))
if "OD_TURN_AT" in env: put("turn-at", text(env["OD_TURN_AT"]))
if "OD_VOICE_LINK" in env: put("voice-link", text(env["OD_VOICE_LINK"]))
open(p, "w").write(s)
PY

# ── the heartbeat, and an hourly push ──
# Every 5 minutes the heartbeat; every hour a full push as well. Turns push on
# their own (checkpoint-command), but a lobby can sit for days with no turn,
# and the server's session token -- which it renews itself, and which lasts 12
# hours -- would otherwise only ever be saved stale. A restart would then
# restore a token the account service has forgotten.
if [ -n "${OD_CAMPAIGN_REPO:-}" ]; then
    # And only when something changed: a push clones the repository first --
    # the world, twice, a few MB and growing -- and the free plan meters
    # bandwidth (5 GB a month). Unchanged, the hour costs a heartbeat.
    # `|| true`, and `set +e` in the loop: this script runs under `set -e` and
    # `pipefail`, and on a NEW campaign saves/ is empty, so cat fails -- which
    # killed this whole loop before its first heartbeat. No heartbeat, and the
    # lock looks abandoned: a redeploy then started a second server on the same
    # campaign while the first was still running it.
    fingerprint() { cat "$data/account.json" "$cfg" "$data"/saves/* 2>/dev/null | cksum || true; }
    ( set +e; n=0; last="$(fingerprint)"; while sleep "${OD_BEAT_SECONDS:-300}"; do
          n=$((n + 1))
          if [ $((n % 12)) = 0 ] && [ "$(fingerprint)" != "$last" ]; then
              /opt/od/campaign_sync.sh push >/dev/null 2>&1 && last="$(fingerprint)"
          else
              /opt/od/campaign_sync.sh heartbeat >/dev/null 2>&1 || true
          fi
          # Console commands the host left in the repository (no shell on a
          # free plan): see `campaign_sync.sh commands`.
          /opt/od/campaign_sync.sh commands 2>&1 | grep -v '^$' || true
      done ) &
    beat_pid=$!
fi

# ── run ──
# Not exec: when the server stops, the campaign has to be pushed and the lock
# released, or the next instance waits for a host that is already gone.
stage "loading the world"
set +e
"$bin" --config "$cfg" --data "$data" < /dev/null &
srv=$!
trap 'stage "stopping"; kill -TERM "$srv" 2>/dev/null' TERM INT
( # The stage, once the server says it is open.
  for _ in $(seq 1 600); do
      kill -0 "$srv" 2>/dev/null || exit 0
      [ -n "$(ls "$data"/saves/*.odhost 2>/dev/null)" ] && { stage "serving"; exit 0; }
      sleep 2
  done ) &
wait "$srv"; rc=$?
# A SIGTERM interrupts the first wait; wait again for the save.
kill -0 "$srv" 2>/dev/null && { wait "$srv"; rc=$?; }
if [ -n "${OD_CAMPAIGN_REPO:-}" ]; then
    kill "${beat_pid:-0}" 2>/dev/null
    # Everything the server just wrote, then the lock: the next instance pulls
    # the moment the lock is gone, so the order matters.
    stage "saving the campaign"
    /opt/od/campaign_sync.sh push || true
    /opt/od/campaign_sync.sh release || true
fi
kill "${health_pid:-0}" 2>/dev/null
exit "$rc"
