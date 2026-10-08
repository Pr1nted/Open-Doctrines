#!/usr/bin/env bash
# A month-long campaign survives its server being switched off.
#
#   tests/campaign_restart_test.sh [build-dir]
#
# WHAT IT PROVES
#
# A tournament with a turn a day runs on a machine that will, at some point,
# lose power, reboot for an update, or be rescheduled by whatever hosts it.
# Each of those is a SIGKILL as far as the server is concerned: no shutdown
# path runs. So this test kills the real OpenDoctrinesServer -- not a NetHost
# in a test harness -- with SIGKILL, twice, and requires that:
#
#   1. it comes back to the SAME world, at the same turn, without being told
#      which world (it records load-save itself);
#   2. it asks the account service to REOPEN the code players saved, and says
#      the session is long-form so the service keeps it;
#   3. each returning player is given their OWN country;
#   4. orders submitted before the kill are still in after it;
#   5. a turn open at the kill keeps the deadline it was announced with --
#      a restart gives nobody extra time;
#   6. a turn whose deadline passed while the server was down resolves once,
#      after a short grace, rather than instantly or never;
#   7. turn numbers carry on from where they were (the off-by-one this test
#      found: a reloaded world believed it had played one turn fewer).
#
# Everything is local: the stand-in issuer (tests/mock_issuer.mjs) mints
# stable identities for dev-alice and dev-bob, and nothing touches a real
# account or the network. Needs node, OpenDoctrinesServer and CampaignClient.

set -u
. "$(dirname "$0")/campaign_lib.sh"
campaign_setup "${1:-$campaign_root/build}" campaigntest
campaign_config turn-seconds=40 resume-grace-seconds=6 \
    auto.start-at-players=2 auto.start-min-players=2 \
    voice-link='"https://discord.gg/odtest"'


echo "=== a campaign, and the server killed under it ==="

start_server "$work/run1.log" || { bad "the server opens a session"; cat "$work/run1.log" | tail -20; exit 1; }
ok "the server opens a session"
check "it records the world so a restart resumes it" \
      "grep -q '\"load-save\": \"Dedicated.odsv\"' '$cfg'"

# Alice claims and stays; Bob claims, which starts the game, and submits.
client dev-alice --claim-index 0 --seconds 120 > "$work/alice1.log" 2>&1 &
alice_pid=$!; pids+=("$alice_pid")
for _ in $(seq 1 100); do grep -q "^ROSTER me=[1-9]" "$work/alice1.log" && break; sleep 0.1; done
client dev-bob --claim-index 1 --submit --marker before-kill --seconds 30 \
       --until ACKED > "$work/bob1.log" 2>&1
check "players are sent the lobby map" "grep -q '^LOBBYMAP w=512' '$work/bob1.log'" \
      "$(grep LOBBYMAP "$work/bob1.log")"
check "players are sent the voice link" "grep -q '^VOICE https://discord.gg/odtest' '$work/bob1.log'"
check "the game starts and turn 1 opens" "grep -q '^TURN 1 ' '$work/bob1.log'"
alice_cid="$(sed -n 's/^ROSTER me=\([1-9][0-9]*\).*/\1/p' "$work/alice1.log" | head -1)"
bob_cid="$(sed -n 's/^ROSTER me=\([1-9][0-9]*\).*/\1/p' "$work/bob1.log" | head -1)"
check "both hold a country, and not the same one" \
      "[ -n '$alice_cid' ] && [ -n '$bob_cid' ] && [ '$alice_cid' != '$bob_cid' ]"

for _ in $(seq 1 50); do [ -f "$save.odorders" ] && break; sleep 0.1; done
check "Bob's orders are on disk before the deadline" \
      "grep -q '$(hexof '{"marker":"before-kill"}')' '$save.odorders'"
deadline1="$(sed -n 's/.*"turnDeadlineMs": \([0-9]*\).*/\1/p' "$save.odhost")"
check "the open turn's deadline is recorded as a wall-clock time" "[ -n '$deadline1' ] && [ '$deadline1' -gt 0 ]"

# ── power cut ──
kill -9 "$server_pid"; wait "$server_pid" 2>/dev/null
sleep 2
kill -9 "$alice_pid" 2>/dev/null

start_server "$work/run2.log" || { bad "the server comes back"; tail -20 "$work/run2.log"; exit 1; }
ok "the server comes back after SIGKILL"
check "it resumes the same world" "grep -q 'resuming Dedicated.odsv' '$work/run2.log'"
check "it goes straight back into the game" "grep -q 'Resumed the campaign at turn 1' '$work/run2.log'" \
      "$(grep -i 'resum' "$work/run2.log" | tail -3)"
stats="$(curl -s "$issuer/session-stats")"
check "it asked the service to reopen the saved code" \
      "printf '%s' '$stats' | grep -q '\"reopen\":\"TEST-GAME\"'" "$stats"
check "and said the session is long-lived" \
      "printf '%s' '$stats' | grep -q '\"longForm\":true'" "$stats"

client dev-bob --seconds 20 --until "TURN" > "$work/bob2.log" 2>&1
check "Bob gets his own country back" "grep -q '^ROSTER me=$bob_cid ' '$work/bob2.log'" \
      "$(grep ROSTER "$work/bob2.log")"
check "his orders from before the kill are still in" \
      "grep -q '^ROSTER me=$bob_cid submitted=1' '$work/bob2.log'" "$(grep ROSTER "$work/bob2.log")"
left="$(sed -n 's/^TURN 1 deadline_ms=\([0-9]*\).*/\1/p' "$work/bob2.log" | head -1)"
now_ms=$(( $(date +%s) * 1000 ))
check "turn 1 kept its original deadline (no free extension)" \
      "[ -n '$left' ] && [ '$left' -gt 0 ] && [ \$(( now_ms + left )) -le \$(( deadline1 + 2000 )) ]" \
      "left=$left now=$now_ms deadline=$deadline1"

client dev-alice --seconds 20 --until "TURN" > "$work/alice2.log" 2>&1
check "Alice gets her own country back" "grep -q '^ROSTER me=$alice_cid ' '$work/alice2.log'" \
      "$(grep ROSTER "$work/alice2.log")"

# Turn 1 resolves on its deadline with nobody connected; turn 2 opens.
for _ in $(seq 1 600); do grep -q "turn 1 resolved" "$work/run2.log" && break; sleep 0.1; done
check "turn 1 resolves on its deadline" "grep -q 'turn 1 resolved' '$work/run2.log'"
check "and the resolved turn is in the save" "unzip -l '$save' | grep -q 'turns/t_00001.dat'"
check "its orders file is gone once they are in the world" "[ ! -f '$save.odorders' ]"

# ── a deadline that passes while the server is down ──
for _ in $(seq 1 50); do grep -q '"openTurn": 2' "$save.odhost" && break; sleep 0.1; done
deadline2="$(sed -n 's/.*"turnDeadlineMs": \([0-9]*\).*/\1/p' "$save.odhost")"
kill -9 "$server_pid"; wait "$server_pid" 2>/dev/null
# Sleep until the deadline is in the past.
while [ $(( $(date +%s) * 1000 )) -le $(( deadline2 + 1000 )) ]; do sleep 1; done
start_server "$work/run3.log" || { bad "the server comes back a second time"; exit 1; }
ok "the server comes back with turn 2 overdue"
check "it resumes at turn 2, not turn 1 again" "grep -q 'Resumed the campaign at turn 2' '$work/run3.log'" \
      "$(grep -i 'resum' "$work/run3.log" | tail -2)"
client dev-bob --seconds 15 --until "TURN" > "$work/bob3.log" 2>&1
grace="$(sed -n 's/^TURN 2 deadline_ms=\([0-9]*\).*/\1/p' "$work/bob3.log" | head -1)"
check "the overdue turn gets the grace, not a fresh 40 seconds" \
      "[ -n '$grace' ] && [ '$grace' -le 6000 ]" "deadline_ms=$grace"
for _ in $(seq 1 200); do grep -q "turn 2 resolved" "$work/run3.log" && break; sleep 0.1; done
check "and then resolves exactly once" "[ \$(grep -c 'turn 2 resolved' '$work/run3.log') -eq 1 ]"
check "turn 3 follows it (no number reused)" \
      "for _ in \$(seq 1 50); do grep -q '\"openTurn\": 3' '$save.odhost' && break; sleep 0.1; done; grep -q '\"openTurn\": 3' '$save.odhost'"

# A server run as a service has no terminal; commands come from a file.
printf 'say from the command file\n' >> "$work/commands.txt"
for _ in $(seq 1 50); do grep -q "from the command file" "$work/run3.log" && break; sleep 0.1; done
check "a command appended to commands.txt runs" "grep -q '\[Server\] from the command file' '$work/run3.log'"

# A polite stop writes everything and exits 0.
kill -TERM "$server_pid"; wait "$server_pid"; rc=$?
check "SIGTERM stops it cleanly" "[ '$rc' -eq 0 ]" "exit $rc"

echo
if [ "$fail" = 0 ]; then echo "campaign restart: all checks passed"; else echo "campaign restart: FAILED"; fi
exit "$fail"
