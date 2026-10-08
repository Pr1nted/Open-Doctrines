#!/usr/bin/env bash
# A relayed dedicated server (no port, players join with the code alone) keeps
# its players' countries across a SIGKILL, the same as a listening one.
#
#   tests/campaign_relay_test.sh [build-dir]
#
# The relay is the stand-in in tests/mock_issuer.mjs, which forwards the
# identity in each player's ticket as the real one does after checking it.
set -u
. "$(dirname "$0")/campaign_lib.sh"
campaign_setup "${1:-$campaign_root/build}" campaignrelay
campaign_config relay=true turn-seconds=300 auto.start-at-players=2 auto.start-min-players=2

relayed() { local who="$1"; shift; "$cli" --issuer "$issuer" --address "" --code TEST-GAME --token "$who" "$@"; }

echo "=== a relayed campaign, and the server killed under it ==="
start_server "$work/r1.log" || { bad "the relayed server opens"; tail -20 "$work/r1.log"; exit 1; }
ok "the relayed server opens"
relayed dev-alice --claim-index 0 --seconds 60 > "$work/alice1.log" 2>&1 &
pids+=("$!")
for _ in $(seq 1 100); do grep -q "^ROSTER me=[1-9]" "$work/alice1.log" && break; sleep 0.1; done
relayed dev-bob --claim-index 1 --submit --seconds 30 --until ACKED > "$work/bob1.log" 2>&1
check "players reach it through the relay and the game starts" "grep -q '^TURN 1 ' '$work/bob1.log'"
bob_cid="$(sed -n 's/^ROSTER me=\([1-9][0-9]*\).*/\1/p' "$work/bob1.log" | head -1)"
check "Bob's orders are acknowledged" "grep -q '^ACKED 1' '$work/bob1.log'"

kill -9 "$server_pid"; wait "$server_pid" 2>/dev/null
start_server "$work/r2.log" || { bad "the relayed server comes back"; exit 1; }
check "it comes back into the game" "grep -q 'Resumed the campaign at turn 1' '$work/r2.log'"
relayed dev-bob --seconds 20 --until TURN > "$work/bob2.log" 2>&1
check "Bob is a player again, not a spectator" "grep -q '^WELCOMED peer=[0-9]* spectator=0' '$work/bob2.log'"
check "with his own country" "grep -q '^ROSTER me=$bob_cid ' '$work/bob2.log'" "$(grep ROSTER "$work/bob2.log")"
check "and his orders still in" "grep -q '^ROSTER me=$bob_cid submitted=1' '$work/bob2.log'"

# ── the relay drops the host, and the host comes back by itself ──
curl -s -X POST "$issuer/relay-drop/TEST-GAME" >/dev/null
for _ in $(seq 1 300); do grep -q "Relay connection restored" "$work/r2.log" && break; sleep 0.1; done
check "a dropped relay is redialled, not fatal" "grep -q 'Relay connection restored' '$work/r2.log'" \
      "$(grep -iE 'relay' "$work/r2.log" | tail -3)"
check "and the server is still running" "kill -0 $server_pid 2>/dev/null"
relayed dev-bob --seconds 20 --until TURN > "$work/bob3.log" 2>&1
check "Bob gets back in through it, to his country" "grep -q '^ROSTER me=$bob_cid ' '$work/bob3.log'" \
      "$(cat "$work/bob3.log" | tail -3)"

od_stop_server "$server_pid" "$work/commands.txt"; wait "$server_pid"; rc=$?
check "a polite stop exits cleanly" "[ '$rc' -eq 0 ]" "exit $rc"
echo
if [ "$fail" = 0 ]; then echo "campaign relay: all checks passed"; else echo "campaign relay: FAILED"; fi
exit "$fail"
