#!/usr/bin/env bash
# A real onion, over the real Tor network: a dedicated server publishes itself
# as an onion service, and a player joins it through a second Tor client.
#
#   tests/tor_live_test.sh [build-dir]
#
# Then a second player joins with no Tor running at all and only the bundle a
# release carries: the game must start that Tor itself.
#
# NOT in run_all.sh: it needs the internet, and the Tor network decides how
# long it takes (a minute or two is normal; publishing a new onion can take
# longer). tests/socks5_test.cpp is the offline half.
#
# Uses the release bundle (tools/fetch_tor.py; set TOR_ARCHIVE to a downloaded
# tor-expert-bundle tarball to skip the download), so no installed tor needed.
set -u
. "$(dirname "$0")/campaign_lib.sh"
campaign_setup "${1:-$campaign_root/build}" torlive
fetch=(python3 "$campaign_root/tools/fetch_tor.py")
[ -n "${TOR_ARCHIVE:-}" ] && fetch+=(--archive "$TOR_ARCHIVE")
"${fetch[@]}" "$work/data" || { echo "could not fetch the Tor bundle"; exit 1; }
tor="$work/data/tor/tor"
campaign_config tunnel='"tor"' turn-seconds=600 auto.start-at-players=1 auto.start-min-players=1

echo "=== a server on an onion, a player through Tor ==="
start_server "$work/server.log" || { bad "the server opens a session"; tail -20 "$work/server.log"; exit 1; }
ok "the server opens a session"
addr=""
for _ in $(seq 1 600); do
    addr="$(sed -n 's/.*address: \(ws:\/\/[a-z2-7]*\.onion\).*/\1/p' "$work/server.log" | head -1)"
    [ -n "$addr" ] && break
    grep -q "tunnel failed" "$work/server.log" && break
    sleep 1
done
check "it publishes an onion address" "[ -n '$addr' ]" "$(grep -iE 'tor|tunnel' "$work/server.log" | tail -3)"
[ -n "$addr" ] || exit 1
echo "      $addr"
check "the onion keys are kept with the campaign" "[ -f '$work/data/onion-service/onion/hostname' ]"

# A separate Tor client for the player, on its own port and directory, the way
# a player's machine would have one.
sp=$(( 30000 + RANDOM % 20000 ))
mkdir -p "$work/playertor"
"$tor" --SocksPort "$sp" --DataDirectory "$work/playertor" --Log "notice stdout" > "$work/playertor.log" 2>&1 &
pids+=("$!")
for _ in $(seq 1 180); do grep -q "Bootstrapped 100%" "$work/playertor.log" && break; sleep 1; done
check "the player's Tor bootstraps" "grep -q 'Bootstrapped 100%' '$work/playertor.log'"

# A freshly published onion can take a while to be findable; try for up to
# five minutes, as a player pressing Join again would.
joined=0
for attempt in $(seq 1 10); do
    "$cli" --issuer "$issuer" --address "$addr" --code TEST-GAME --token dev-bob \
           --tor-port "$sp" --claim-index 0 --submit --seconds 90 --until ACKED > "$work/bob.log" 2>&1
    grep -q "^ACKED" "$work/bob.log" && { joined=1; break; }
    echo "      attempt $attempt: $(grep -E 'REJECTED|DISCONNECTED|TIMEOUT' "$work/bob.log" | head -1)"
    sleep 20
done
check "a player joins the onion through Tor" "[ '$joined' = 1 ]" "$(tail -3 "$work/bob.log")"
check "and plays: gets a country, a turn, and their orders in" \
      "grep -q '^ROSTER me=[1-9]' '$work/bob.log' && grep -q '^TURN 1' '$work/bob.log'"

# ── a player with no Tor running: the game starts the bundled one ──
# The game is under way by now, so Carol comes in as a spectator; what is
# being proved is that she reaches the onion and is sent the world.
for p in 9050 9150; do
    if (exec 3<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null; then
        echo "      note: something answers on $p, so the game would use it; skipping the auto-start half"
        skip_auto=1
    fi
done
if [ -z "${skip_auto:-}" ]; then
    mkdir -p "$work/carol/tor"
    cp -R "$work/data/tor/." "$work/carol/tor/"
    "$cli" --issuer "$issuer" --address "$addr" --code TEST-GAME --token dev-carol \
           --tor-data "$work/carol" --seconds 240 --until SNAPSHOT \
           > "$work/carol.log" 2>&1
    check "with no Tor running, the game starts its own and joins" \
          "grep -q '^WELCOMED' '$work/carol.log' && grep -q '^SNAPSHOT' '$work/carol.log'" \
          "$(tail -3 "$work/carol.log")"
    check "it kept that Tor's state in its own data directory" "[ -f '$work/carol/tor-client/cached-certs' ] || [ -f '$work/carol/tor-client/state' ]"
    sleep 1
    check "and that Tor is gone once the game exits" \
          "! pgrep -f -- '$work/carol/tor/tor' >/dev/null"
fi

kill -TERM "$server_pid"; wait "$server_pid"
echo
if [ "$fail" = 0 ]; then echo "tor live: all checks passed"; else echo "tor live: FAILED"; fi
exit "$fail"
