# Shared by the campaign tests: a sandbox data directory, the stand-in issuer,
# a dedicated server config, and helpers to run the server and scripted
# players. Sourced, not run.
#
#   campaign_setup <build-dir> <work-name>   sets: work, issuer, port, cfg, save
#   campaign_config key=value ...            edits server.json
#   start_server <log>                       sets: server_pid
#   client <token> <campaign_client args...>
#
# Everything is local: nothing here touches a real account or the network.

campaign_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail=0
pids=()

ok()    { printf '  %-64s ok\n' "$1"; }
bad()   { printf '  %-64s FAILED\n' "$1"; [ -n "${2:-}" ] && printf '      %s\n' "$2"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1" "${3:-}"; fi; }

campaign_cleanup() { for p in "${pids[@]}"; do kill -9 "$p" 2>/dev/null; done; }
trap campaign_cleanup EXIT

campaign_setup() {
    local build="$1" name="$2"
    srv="$build/OpenDoctrinesServer"
    cli="$build/CampaignClient"
    for b in "$srv" "$cli"; do
        [ -x "$b" ] || { echo "missing $b -- build OpenDoctrinesServer and CampaignClient"; exit 1; }
    done
    command -v node >/dev/null || { echo "node is needed for the stand-in issuer"; exit 1; }

    work="$build/$name"
    rm -rf "$work" && mkdir -p "$work/data/saves"
    # Bulk content by symlink; everything the server writes is its own.
    for e in "$campaign_root/data"/*; do
        local b; b="$(basename "$e")"
        case "$b" in account.json|config.json|servers.json|saves|tools|mods.json) continue ;; esac
        ln -s "$e" "$work/data/$b"
    done

    node "$campaign_root/tests/mock_issuer.mjs" --port 0 > "$work/issuer.log" 2>&1 &
    pids+=("$!")
    for _ in $(seq 1 50); do grep -q "ready on" "$work/issuer.log" && break; sleep 0.1; done
    issuer="$(sed -n 's/.*ready on \(http[^ ]*\).*/\1/p' "$work/issuer.log" | head -1)"
    [ -n "$issuer" ] || { echo "the stand-in issuer did not start"; cat "$work/issuer.log"; exit 1; }

    printf '{"accountIssuer": "%s", "accountAgreed": true}\n' "$issuer" > "$work/data/config.json"
    printf '{"issuer": "%s", "token": "dev-host"}\n' "$issuer" > "$work/data/account.json"

    port=$(( 40000 + RANDOM % 20000 ))
    cfg="$work/server.json"
    save="$work/data/saves/Dedicated.odsv"
    "$srv" --write-config --config "$cfg" --data "$work/data" >/dev/null 2>&1
    campaign_config map='"1914"' port="$port" tunnel='"off"'
}

campaign_config() {
    python3 - "$cfg" "$@" <<'EOF'
import re, sys
p = sys.argv[1]
s = open(p).read()
for kv in sys.argv[2:]:
    key, value = kv.split("=", 1)
    s, n = re.subn(r'"%s": [^,\n]+' % re.escape(key), '"%s": %s' % (key, value), s)
    if n != 1:
        sys.exit("no setting %s in %s" % (key, p))
open(p, "w").write(s)
EOF
}

start_server() {
    "$srv" --config "$cfg" --data "$work/data" < /dev/null > "$1" 2>&1 &
    server_pid=$!
    pids+=("$server_pid")
    for _ in $(seq 1 300); do
        grep -q "session open" "$1" && return 0
        kill -0 "$server_pid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

client() {
    local who="$1"; shift
    "$cli" --issuer "$issuer" --address "127.0.0.1:$port" --code TEST-GAME \
           --token "$who" "$@"
}

hexof() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; }

# Resident memory of a pid in megabytes, from ps (both macOS and Linux).
# Windows is not a POSIX box wearing a bash prompt, and these two helpers are
# where that stops being a detail. Both campaign tests and the soak ran here for
# the first time once the odseal prebuilts landed and the windows-x86 and
# windows-arm64 jobs got past configure; both failed, for reasons that are
# about the harness rather than the server.
od_is_windows() {
    case "$(uname -s 2>/dev/null)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac
}

# RESIDENT SIZE. `ps -o rss=` is MSYS's own ps, which does not report rss for a
# NATIVE Windows process -- it returns nothing at all, so the soak printed
# "memory did not keep climbing (warm  MB, now  MB)" with both numbers empty and
# then failed on the arithmetic. PowerShell can answer for a real Windows
# process, but only if asked by the WINDOWS pid, which is not the pid bash
# holds; `ps -W` is what maps one to the other. Empty output is left empty, and
# the callers decide what an unmeasurable number means.
rss_mb() {
    local v
    v="$(ps -o rss= -p "$1" 2>/dev/null | awk '{printf "%d", $1 / 1024}')"
    if [ -z "$v" ] && od_is_windows; then
        local winpid
        winpid="$(ps -W 2>/dev/null | awk -v p="$1" '$1 == p { print $4; exit }')"
        [ -n "$winpid" ] && v="$(powershell.exe -NoProfile -Command \
            "try { [int]((Get-Process -Id $winpid).WorkingSet64/1MB) } catch { '' }" \
            2>/dev/null | tr -d '\r')"
    fi
    printf '%s' "$v"
}

# A POLITE STOP, by whatever the platform actually has.
#
# `kill -TERM` to a native Windows process is not a signal: MSYS calls
# TerminateProcess, which nothing can catch, so the handler installed by
# signal(SIGTERM, ...) never runs and the process dies with 143 however well
# the server is written. The server's own `stop` command -- "save and shut the
# server down" -- does the same work through the command file, which these
# tests have already proved is read. The property under test is the same
# either way: a polite stop writes everything and exits 0.
#
# $1 = pid, $2 = the commands file the server is watching.
od_stop_server() {
    if od_is_windows && [ -n "${2:-}" ]; then
        printf 'stop\n' >> "$2"
    else
        kill -TERM "$1"
    fi
}
