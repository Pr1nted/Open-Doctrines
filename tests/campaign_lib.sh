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
rss_mb() { ps -o rss= -p "$1" 2>/dev/null | awk '{printf "%d", $1 / 1024}'; }
