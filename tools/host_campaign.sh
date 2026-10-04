#!/usr/bin/env bash
# Run a campaign's dedicated server on THIS computer, started at login and
# restarted if it dies, so a reboot or a crash costs a minute, not the game.
#
#   tools/host_campaign.sh install <dir>    set it up; <dir> holds server.json
#   tools/host_campaign.sh uninstall        stop it and remove the service
#   tools/host_campaign.sh status           is it running, and the last log lines
#   tools/host_campaign.sh cmd <dir> <...>   a console command: status, say, seat, deadline...
#   tools/host_campaign.sh log              follow the server's log
#   tools/host_campaign.sh run <dir>        what the service runs (foreground)
#
# <dir> is the campaign's own folder: server.json, plus a data/ directory the
# server writes its world, config.json and account.json into. `install` creates
# it from this checkout's data/ (by symlink) if it does not exist, copying your
# signed-in account and server credential so players keep their identities.
#
# What happens while this computer is OFF is the campaign's choice, not this
# script's: the turn deadline is wall-clock time, so a turn that comes due
# while the machine is off resolves a couple of minutes after it is back, and
# nothing is lost. To keep the game moving while it is off, run the same
# campaign on a host that stays up -- see docs/tournaments.md -- with
# OD_CAMPAIGN_REPO set here too, so the two hand it back and forth.
#
# macOS: a LaunchAgent, under caffeinate so the Mac does not sleep under it.
# Linux: a systemd user unit (`loginctl enable-linger $USER` keeps it running
#        while you are logged out).
# Windows: not scripted -- see docs/tournaments.md for the Task Scheduler line.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
label="com.opendoctrines.campaign"
cmd="${1:-}"

find_server() {
    for c in "$root/build/OpenDoctrinesServer" "$root/cmake-build-release/OpenDoctrinesServer" \
             "$root/cmake-build-debug/OpenDoctrinesServer"; do
        [ -x "$c" ] && { echo "$c"; return; }
    done
    echo "no OpenDoctrinesServer -- build it: cmake --build build --target OpenDoctrinesServer" >&2
    exit 1
}

prepare() {                        # make <dir> a campaign folder
    local dir="$1"
    mkdir -p "$dir/data/saves"
    for e in "$root/data"/*; do
        local b; b="$(basename "$e")"
        case "$b" in saves|config.json|account.json|servers.json|mods.json) continue ;; esac
        [ -e "$dir/data/$b" ] || ln -s "$e" "$dir/data/$b"
    done
    # Your identity as a host: the token and, above all, the server credential.
    # A new credential would make every returning player a stranger.
    [ -f "$dir/data/account.json" ] || { [ -f "$root/data/account.json" ] &&
        cp "$root/data/account.json" "$dir/data/account.json" && chmod 600 "$dir/data/account.json"; } || true
    if [ ! -f "$dir/data/config.json" ] && [ -f "$root/data/config.json" ]; then
        python3 - "$root/data/config.json" "$dir/data/config.json" <<'PY'
import json, sys
src = json.load(open(sys.argv[1]))
keep = {k: src[k] for k in ("accountIssuer", "serverCredential", "accountAgreed") if k in src}
json.dump(keep, open(sys.argv[2], "w"), indent=2)
PY
    fi
    fresh=0
    if [ ! -f "$dir/server.json" ]; then
        "$(find_server)" --write-config --config "$dir/server.json" --data "$dir/data" >/dev/null
        fresh=1
    fi
}

case "$cmd" in
run)
    dir="$(cd "${2:?campaign folder}" && pwd)"
    export OD_DATA_DIR="$dir/data" OD_SERVER_CONFIG="$dir/server.json"
    if [ -n "${OD_CAMPAIGN_REPO:-}" ]; then
        "$root/tools/campaign_sync.sh" pull
        # The server pushes after every turn; this says so in the config.
        python3 - "$dir/server.json" "$root/tools/campaign_sync.sh push" <<'PY'
import re, sys
p, cmd = sys.argv[1], sys.argv[2]
s = open(p).read()
s = re.sub(r'"checkpoint-command": [^,\n]+', '"checkpoint-command": "%s"' % cmd, s)
open(p, "w").write(s)
PY
    fi
    srv="$(find_server)"
    trap '[ -n "${OD_CAMPAIGN_REPO:-}" ] && "$root/tools/campaign_sync.sh" release || true' EXIT
    "$srv" --config "$dir/server.json" --data "$dir/data" < /dev/null
    ;;
install)
    dir="${2:?campaign folder}"
    mkdir -p "$dir" && dir="$(cd "$dir" && pwd)"
    prepare "$dir"
    # A brand-new campaign is NOT started: the first start creates the world
    # from whatever server.json says, and the defaults are not a tournament.
    if [ "$fresh" = 1 ] && [ -z "${OD_CAMPAIGN_REPO:-}" ]; then
        echo "wrote $dir/server.json. Set the map, turn-seconds (86400 = a day),"
        echo "turn-at, voice-link and the rest there (docs/hosting-a-week.md), then run"
        echo "    $0 install $dir"
        echo "again to start it."
        exit 0
    fi
    me="$root/tools/host_campaign.sh"
    case "$(uname -s)" in
    Darwin)
        plist="$HOME/Library/LaunchAgents/$label.plist"
        mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
        cat > "$plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$label</string>
  <key>ProgramArguments</key><array>
    <string>/usr/bin/caffeinate</string><string>-i</string>
    <string>/bin/bash</string><string>$me</string><string>run</string><string>$dir</string>
  </array>
  <key>EnvironmentVariables</key><dict>
    <key>PATH</key><string>/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin</string>
    ${OD_CAMPAIGN_REPO:+<key>OD_CAMPAIGN_REPO</key><string>$OD_CAMPAIGN_REPO</string>}
  </dict>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>30</integer>
  <key>StandardOutPath</key><string>$HOME/Library/Logs/OpenDoctrinesCampaign.log</string>
  <key>StandardErrorPath</key><string>$HOME/Library/Logs/OpenDoctrinesCampaign.log</string>
</dict></plist>
EOF
        chmod 600 "$plist"
        launchctl unload "$plist" 2>/dev/null || true
        launchctl load "$plist"
        echo "installed: starts at login, restarts if it stops."
        echo "log: $HOME/Library/Logs/OpenDoctrinesCampaign.log"
        ;;
    Linux)
        unit="$HOME/.config/systemd/user/opendoctrines-campaign.service"
        mkdir -p "$(dirname "$unit")"
        cat > "$unit" <<EOF
[Unit]
Description=OpenDoctrines campaign server
After=network-online.target

[Service]
ExecStart=/bin/bash $me run $dir
${OD_CAMPAIGN_REPO:+Environment=OD_CAMPAIGN_REPO=$OD_CAMPAIGN_REPO}
Restart=always
RestartSec=30
KillSignal=SIGTERM
TimeoutStopSec=120

[Install]
WantedBy=default.target
EOF
        chmod 600 "$unit"
        systemctl --user daemon-reload
        systemctl --user enable --now opendoctrines-campaign.service
        echo "installed. Logs: journalctl --user -u opendoctrines-campaign -f"
        echo "To keep it running while logged out: loginctl enable-linger $USER"
        ;;
    *) echo "not scripted on this OS; see docs/tournaments.md"; exit 1 ;;
    esac
    ;;
uninstall)
    case "$(uname -s)" in
    Darwin)
        plist="$HOME/Library/LaunchAgents/$label.plist"
        launchctl unload "$plist" 2>/dev/null || true
        rm -f "$plist" ;;
    Linux)
        systemctl --user disable --now opendoctrines-campaign.service 2>/dev/null || true
        rm -f "$HOME/.config/systemd/user/opendoctrines-campaign.service"
        systemctl --user daemon-reload ;;
    esac
    echo "removed. The campaign folder is untouched."
    ;;
cmd)
    # A console command for the running server, which as a service has no
    # terminal: appended to commands.txt, run within a second, answered in the
    # log -- which is shown for a few seconds afterwards.
    dir="${2:?campaign folder}"; shift 2
    [ $# -gt 0 ] || { echo "usage: $0 cmd <dir> <command...>   e.g. status, seat Bob France"; exit 2; }
    case "$(uname -s)" in
        Darwin) log="$HOME/Library/Logs/OpenDoctrinesCampaign.log" ;;
        *)      log="" ;;
    esac
    printf '%s\n' "$*" >> "$dir/commands.txt"
    if [ -n "$log" ] && [ -f "$log" ]; then
        before=$(wc -l < "$log")
        sleep 3
        tail -n +"$((before + 1))" "$log"
    else
        sleep 3
        journalctl --user -u opendoctrines-campaign --since "-10 s" --no-pager -o cat 2>/dev/null || true
    fi
    ;;
log)
    case "$(uname -s)" in
        Darwin) tail -f "$HOME/Library/Logs/OpenDoctrinesCampaign.log" ;;
        *)      journalctl --user -u opendoctrines-campaign -f -o cat ;;
    esac
    ;;
status)
    case "$(uname -s)" in
    Darwin) launchctl list | grep "$label" || echo "not running"
            tail -n 15 "$HOME/Library/Logs/OpenDoctrinesCampaign.log" 2>/dev/null || true ;;
    Linux)  systemctl --user status opendoctrines-campaign.service --no-pager | head -20 || true ;;
    esac
    ;;
*) sed -n '2,15p' "$0"; exit 2 ;;
esac
