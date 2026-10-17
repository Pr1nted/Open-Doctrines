#!/usr/bin/env bash
# A hosted game's players decide their own country's affairs, and answer what
# they are asked.
#
#   tests/campaign_choices_test.sh [build-dir]
#
# WHAT IT PROVES
#
#   1. What a player decides that is not an order reaches the server: a
#      research budget and a pacification budget, a research project, a
#      doctrine, what they publish about themselves, and a war they declare.
#      Each must be in the next state the server sends. Before this, none of
#      them left the client: once the AI stopped playing human seats, a
#      remote player's research budget sat wherever the AI had left it.
#   2. A request aimed at a remote player is THEIRS to answer. Alice asks Bob
#      for an alliance; Bob's client is sent it, answers yes, and the alliance
#      is in the next state. The server's AI used to answer for him.
#   3. A request nobody answers lapses without prejudice. Alice and Carol ally;
#      Alice then calls Carol to arms, and Carol says nothing. The call must
#      lapse -- Carol is not dragged into the war -- and the alliance must
#      still stand. Ending an alliance over a closed laptop would punish
#      absence as though it were betrayal.
#
# Local only: the stand-in issuer, no account, no network.

set -u
. "$(dirname "$0")/campaign_lib.sh"
campaign_setup "${1:-$campaign_root/build}" campaignchoices
campaign_config map='"map"' turn-seconds=12 auto.start-at-players=3 auto.start-min-players=3

# Three countries with no treaties between them on this map, and a fourth for
# Alice to go to war with that has no allies to bring in. By ISO, looked up in
# the map itself, so a regenerated map does not silently move the test onto
# somebody else.
read -r A B C <<EOF
$(python3 - "$campaign_root/data/STDmaps/map.odmap" <<'PY'
import json, sys, zipfile
c = json.loads(zipfile.ZipFile(sys.argv[1]).read("countries.json"))
iso = {v["iso_a3"]: v["id"] for v in c.values()}
print(iso["MEX"], iso["ZAF"], iso["DZA"])
PY
)
EOF
[ -n "${C:-}" ] || { bad "the map has the countries this test plays"; exit 1; }

start_server "$work/server.log" || { bad "the server opens"; tail -20 "$work/server.log"; exit 1; }
ok "the server opens"

turns=12
alog="$work/alice.log" blog="$work/bob.log" clog="$work/carol.log"
aorders="$work/alice.json"
cat > "$aorders" <<'JSON'
{"budgets": {"research": 0.05, "pacification": 0.02},
 "research": [{"node": "mon_basics", "share": 100, "auto": false}],
 "policies": [{"op": "enact", "id": "patriotic_education"}],
 "disclosure": 3,
 "diplomacy": [{"action": "declare_war", "target": "CUB"},
               {"action": "request_alliance", "target": "ZAF"},
               {"action": "request_alliance", "target": "DZA"}]}
JSON

client dev-alice --claim "$A" --submit --submit-after 1 --orders-file "$aorders" \
       --seconds 400 --until "TURNSTATE turn=$turns" > "$alog" 2>&1 &
a=$!; pids+=("$a")
# ONE AT A TIME. Each client first asks the stand-in account service for a
# join ticket, and three at once on a loaded windows-x64 runner timed one of
# them out ("could not reach 127.0.0.1 in time") -- the whole test then failed
# for want of a player, not for anything a player decided. Each joins before
# the next starts, as campaign_det_test does.
wait_for_line "$alog" '^ROSTER me=[1-9]' 600 "$a"
client dev-bob --claim "$B" --submit --submit-after 1 --answer accept \
       --seconds 400 --until "TURNSTATE turn=$turns" > "$blog" 2>&1 &
b=$!; pids+=("$b")
wait_for_line "$blog" '^ROSTER me=[1-9]' 600 "$b"
# Carol accepts an alliance and nothing else: a call to arms is left unanswered.
client dev-carol --claim "$C" --submit --submit-after 1 --answer accept \
       --answer-actions request_alliance \
       --seconds 400 --until "TURNSTATE turn=$turns" > "$clog" 2>&1 &
c=$!; pids+=("$c")

# Once Alice and Carol are allies, Alice calls her to arms against Cuba.
if wait_for_line "$alog" '^TURNSTATE .*allies=.*DZA-MEX' 3000 "$a"; then
    python3 - "$aorders" <<'PY'
import json, sys
p = sys.argv[1]
o = json.load(open(p))
o["diplomacy"] = [{"action": "call_to_arms", "target": "DZA"}]
json.dump(o, open(p, "w"))
PY
fi
wait "$a" "$b" "$c" 2>/dev/null

field() { sed -n "s/.* $1=\([^ ]*\).*/\1/p"; }
last="$(grep '^TURNSTATE turn=' "$alog" | tail -1)"

echo "=== what alice decided reached the server ==="
check "alice played $turns turns" "grep -q '^TURNSTATE turn=$turns ' '$alog'" "$(tail -3 "$alog")"
check "alice holds Mexico" "[ \"\$(printf '%s' \"\$last\" | field my)\" = '$A' ]" "$last"
alloc="$(printf '%s' "$last" | field alloc)"
pac="$(printf '%s' "$last" | field pac)"
check "her research budget is the one she set (5%)" \
      "python3 -c 'import sys; sys.exit(0 if abs(float(\"$alloc\") - 0.05) < 1e-4 else 1)'" "alloc=$alloc"
check "and her pacification budget (2%)" \
      "python3 -c 'import sys; sys.exit(0 if abs(float(\"$pac\") - 0.02) < 1e-4 else 1)'" "pac=$pac"
check "her research group works on what she chose" \
      "grep '^TURNSTATE turn=[1-9]' '$alog' | grep -q 'groups=mon_basics|'" \
      "$(grep '^TURNSTATE' "$alog" | field groups | sort -u | tr '\n' ' ')"
check "her doctrine is in force or being implemented" \
      "printf '%s' \"\$last\" | field policies | tr ',' '\n' | grep -qx patriotic_education" \
      "policies=$(printf '%s' "$last" | field policies)"
check "she publishes what she said she would" "[ \"\$(printf '%s' \"\$last\" | field disclosure)\" = 3 ]" \
      "disclosure=$(printf '%s' "$last" | field disclosure)"
check "her war on Cuba was declared" "printf '%s' \"\$last\" | field wars | tr ',' '\n' | grep -qx CUB-MEX" \
      "wars=$(printf '%s' "$last" | field wars | head -c 300)"

echo "=== bob is asked, and answers ==="
check "bob was sent alice's request" "grep -q '^TURNSTATE .*asked=[^ ]*request_alliance:MEX' '$blog'" \
      "$(grep '^TURNSTATE' "$blog" | field asked | sort -u | tr '\n' ' ')"
check "bob answered it" "grep -q '^ANSWERED .*request_alliance accept' '$blog'"
check "and the server heard him, not its AI" "grep -q '^\[MPANSWER\] ZAF accepts request_alliance from MEX' '$work/server.log'"
check "the alliance is in the state everyone is sent" \
      "grep '^TURNSTATE' '$alog' | field allies | tr ',' '\n' | grep -qx MEX-ZAF"

echo "=== carol is called, and says nothing ==="
check "carol allied with alice" "grep -q '^\[MPANSWER\] DZA accepts request_alliance from MEX' '$work/server.log'"
check "alice called her to arms" "grep -q '^\[MPINBOX\] MEX -> DZA .*call_to_arms' '$work/server.log'" \
      "$(grep 'MPINBOX\|calls .* to arms\|Refused a call' "$work/server.log" | head -5)"
check "carol was sent the call" "grep -q '^TURNSTATE .*asked=[^ ]*call_to_arms:MEX' '$clog'"
check "carol did not answer it" "! grep -q '^ANSWERED .*call_to_arms' '$clog'"
check "so it lapsed on the server" "grep -q '^\[MPANSWER\] DZA did not answer call_to_arms from MEX' '$work/server.log'"
clast="$(grep '^TURNSTATE turn=' "$clog" | tail -1)"
check "carol is not at war with Cuba" "! printf '%s' \"\$clast\" | field wars | tr ',' '\n' | grep -qx CUB-DZA" \
      "wars=$(printf '%s' "$clast" | field wars | head -c 300)"
check "and the alliance stands" "printf '%s' \"\$clast\" | field allies | tr ',' '\n' | grep -qx DZA-MEX" \
      "allies=$(printf '%s' "$clast" | field allies | head -c 300)"

kill -TERM "$server_pid"; wait "$server_pid" 2>/dev/null
echo
if [ "$fail" = 0 ]; then echo "campaign choices: all checks passed"; else echo "campaign choices: FAILED"; fi
exit "$fail"
