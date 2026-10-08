#!/usr/bin/env bash
# Turn a fresh Linux VM into an always-on OpenDoctrines server, with the
# language model running on the same machine.
#
#   tools/server_box.sh [options]          run ON the VM, from a checkout
#
# Written for Oracle Cloud's Always Free Arm VM (4 cores, 24 GB, Ubuntu) --
# see docs/hosting-on-oracle.md -- and works on any Debian or Ubuntu box.
# Safe to run again: each step checks whether it is already done.
#
#   --campaign DIR      the campaign folder               (default ~/od-campaign)
#   --identity DIR      where account.json + config.json from your own game
#                       were copied to                     (default ~/od-identity)
#   --name TEXT         the server's name                 (default "OpenDoctrines")
#   --map ID            map id or .odmap path             (default map = Modern Day)
#   --turn-seconds N    turn length                       (default 86400, a day)
#   --turn-at HH:MM     turns due at this UTC time        (default none)
#   --max-players N                                       (default 16)
#   --model NAME        Ollama model, or "none"           (default llama3.1:8b)
#   --llm-endpoint URL  use this OpenAI-style endpoint instead of installing Ollama
#   --foreground        run the server in this terminal instead of as a service
#   --no-build          use the OpenDoctrinesServer already built
#
# What it leaves running:
#   ollama              the model, on 127.0.0.1:11434 only, kept loaded
#   the server          a systemd user service (tools/host_campaign.sh), hosting
#                       through the relay: no port to open, players join by code
#
# The game is a LOBBY until you start it: nothing starts on its own. Start it
# with   tools/host_campaign.sh cmd ~/od-campaign start
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
campaign="$HOME/od-campaign"
identity="$HOME/od-identity"
name="OpenDoctrines"
map="map"
turn_seconds=86400
turn_at=""
max_players=16
model="llama3.1:8b"
endpoint=""
foreground=0
build=1
given=""          # settings named on this command line; a re-run changes only these

while [ $# -gt 0 ]; do
    case "$1" in
        --campaign) campaign="$2"; shift 2 ;;
        --identity) identity="$2"; shift 2 ;;
        --name) name="$2"; given="$given name"; shift 2 ;;
        --map) map="$2"; given="$given map"; shift 2 ;;
        --turn-seconds) turn_seconds="$2"; given="$given turn-seconds"; shift 2 ;;
        --turn-at) turn_at="$2"; given="$given turn-at"; shift 2 ;;
        --max-players) max_players="$2"; given="$given max-players"; shift 2 ;;
        --model) model="$2"; given="$given llm"; shift 2 ;;
        --llm-endpoint) endpoint="$2"; given="$given llm"; shift 2 ;;
        --foreground) foreground=1; shift ;;
        --no-build) build=0; shift ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "unknown option $1 (see --help)"; exit 2 ;;
    esac
done

say() { printf '\n== %s\n' "$*"; }
sudo_() { if [ "$(id -u)" = 0 ]; then "$@"; else sudo "$@"; fi; }

[ "$(uname -s)" = Linux ] || { echo "this sets up a Linux server; see docs/hosting-a-week.md for others"; exit 1; }

# ── 1. What the build needs ──
if [ "$build" = 1 ]; then
    say "installing build tools"
    if command -v apt-get >/dev/null; then
        sudo_ apt-get update -qq
        # The X and GL headers are for raylib's configure step; the server
        # itself never opens a window.
        sudo_ env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
            build-essential cmake git python3 curl ca-certificates pkg-config zstd \
            libasound2-dev libx11-dev libxrandr-dev libxi-dev libgl1-mesa-dev \
            libglu1-mesa-dev libxcursor-dev libxinerama-dev libwayland-dev libxkbcommon-dev \
            >/dev/null
    else
        echo "not a Debian/Ubuntu system: install cmake, a C++ compiler, git, python3, curl"
        echo "and the X11/GL development headers yourself, then run with --no-build after building."
        exit 1
    fi

    say "building the server (a few minutes the first time)"
    cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$root/build" --target OpenDoctrinesServer -j"$(nproc)" >/dev/null
fi
[ -x "$root/build/OpenDoctrinesServer" ] || { echo "no build/OpenDoctrinesServer"; exit 1; }

# ── 2. The language model, on this machine ──
#
# On the first setup, or when a model is named. A plain re-run leaves the
# model the campaign already uses alone instead of fetching the default.
fresh=0
[ -f "$campaign/server.json" ] || fresh=1
mem_gb=$(awk '/MemTotal/ {printf "%d", $2 / 1048576}' /proc/meminfo)
if [ "$fresh" = 0 ] && [[ " $given " != *" llm "* ]]; then
    :
elif [ -z "$endpoint" ] && [ "$model" != none ]; then
    # Roughly what the weights need resident, plus the server and the system.
    need=6
    case "$model" in *:0.5b|*:1b|*:1.5b) need=2 ;; *:3b|*:4b) need=4 ;; *:7b|*:8b) need=7 ;; esac
    if [ "$mem_gb" -lt "$need" ]; then
        echo "this machine has ${mem_gb} GB; $model wants about ${need} GB."
        echo "Use a smaller one (--model qwen2.5:1.5b), --llm-endpoint, or --model none."
        exit 1
    fi
    if ! command -v ollama >/dev/null; then
        say "installing Ollama (ollama.com's own installer)"
        curl -fsSL https://ollama.com/install.sh | sh
    fi
    if command -v systemctl >/dev/null && systemctl list-unit-files ollama.service >/dev/null 2>&1 \
       && [ -d /run/systemd/system ]; then
        # Loopback only, and kept loaded: a reply should not wait for the
        # weights to be read back from disk once a day.
        sudo_ mkdir -p /etc/systemd/system/ollama.service.d
        printf '[Service]\nEnvironment="OLLAMA_HOST=127.0.0.1:11434"\nEnvironment="OLLAMA_KEEP_ALIVE=-1"\n' \
            | sudo_ tee /etc/systemd/system/ollama.service.d/opendoctrines.conf >/dev/null
        sudo_ systemctl daemon-reload
        sudo_ systemctl enable --now ollama >/dev/null 2>&1 || true
        sudo_ systemctl restart ollama
    elif ! curl -fsS http://127.0.0.1:11434/api/tags >/dev/null 2>&1; then
        # No systemd (a container): run it in the background ourselves.
        OLLAMA_HOST=127.0.0.1:11434 OLLAMA_KEEP_ALIVE=-1 nohup ollama serve > "$HOME/ollama.log" 2>&1 &
    fi
    for _ in $(seq 1 60); do curl -fsS http://127.0.0.1:11434/api/tags >/dev/null 2>&1 && break; sleep 1; done
    say "fetching $model (once; several GB for an 8B model)"
    ollama pull "$model"
    endpoint="http://127.0.0.1:11434/v1"
fi

# ── 3. Your identity as a host ──
#
# The server hosts AS YOUR ACCOUNT, and its credential decides who every player
# is on it. Both come from your own game; see docs/hosting-on-oracle.md.
mkdir -p "$campaign/data"
if [ ! -f "$campaign/data/account.json" ]; then
    if [ ! -f "$identity/account.json" ] || [ ! -f "$identity/config.json" ]; then
        echo
        echo "No host identity. Copy account.json and config.json from your own game's"
        echo "data/ folder to $identity/ on this machine, then run this again:"
        echo "    scp data/account.json data/config.json <you>@<this-vm>:od-identity/"
        exit 1
    fi
    cp "$identity/account.json" "$campaign/data/account.json"
    chmod 600 "$campaign/data/account.json"
    python3 - "$identity/config.json" "$campaign/data/config.json" <<'PY'
import json, sys
src = json.load(open(sys.argv[1]))
keep = {k: src[k] for k in ("accountIssuer", "serverCredential", "accountAgreed") if k in src}
json.dump(keep, open(sys.argv[2], "w"), indent=2)
PY
    chmod 600 "$campaign/data/config.json"
fi

# ── 4. The campaign: a lobby on the chosen map, a turn a day ──
#
# Written in full the FIRST time. A re-run (to update the build) changes only
# what was named on its command line, so it never quietly resets a name, a
# map or a schedule somebody set by hand.
say "writing $campaign/server.json"
"$root/tools/host_campaign.sh" install "$campaign" >/dev/null || true   # creates it; does not start
python3 - "$campaign/server.json" "$campaign/data/config.json" "$name" "$map" "$turn_seconds" \
          "$turn_at" "$max_players" "$endpoint" "$model" "$fresh" "$given" <<'PY'
import json, re, sys
cfg, game, name, mapid, turn, turn_at, maxp, endpoint, model, fresh, given = sys.argv[1:12]
fresh = fresh == "1"
given = set(given.split())
s = open(cfg).read()
def put(key, value, always=False):
    global s
    if not (fresh or always or key in given):
        return
    s, n = re.subn(r'"%s": [^,\n]+' % re.escape(key), '"%s": %s' % (key, json.dumps(value)), s)
    if n != 1:
        sys.exit("no setting %s in %s" % (key, cfg))
put("name", name)
put("map", mapid)
put("turn-seconds", int(turn))
put("turn-at", turn_at)
put("max-players", int(maxp))
put("relay", True)                  # no port to open; players join by code
put("tunnel", "off")
put("assignment", "players")
put("absent", "ai")
# A lobby until the host says start: nothing starts by itself.
put("auto.start-at-players", 0)
put("auto.start-after-seconds", 0)
put("auto.return-to-lobby", False)
open(cfg, "w").write(s)

g = json.load(open(game))
if not (fresh or "llm" in given):
    pass
elif endpoint and model != "none":
    g.update({"llmEnabled": True, "llmEndpoint": endpoint, "llmModel": model})
else:
    g["llmEnabled"] = False
json.dump(g, open(game, "w"), indent=2)
PY

# ── 5. Run it ──
if [ "$foreground" = 1 ]; then
    say "running in the foreground (Ctrl-C stops it)"
    exec "$root/tools/host_campaign.sh" run "$campaign"
fi
say "installing the service"
sudo_ loginctl enable-linger "$(id -un)" 2>/dev/null || true
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
"$root/tools/host_campaign.sh" install "$campaign"

say "waiting for the invite code"
code=""
for _ in $(seq 1 180); do
    code="$(journalctl --user -u opendoctrines-campaign --no-pager -o cat 2>/dev/null \
            | sed -n 's/.*session open\. Join code: \([A-Z0-9-]*\).*/\1/p' | tail -1)"
    [ -n "$code" ] && break
    sleep 2
done
if [ -n "$code" ]; then
    echo
    echo "  Invite code: $code"
    echo "  Players: Multiplayer -> Join -> enter the code. It is a lobby until you run"
    echo "      tools/host_campaign.sh cmd $campaign start"
else
    echo "no code yet -- follow the log: tools/host_campaign.sh log"
fi
