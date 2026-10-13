#!/usr/bin/env bash
# A hosted game's players see the game the server is playing, and only the
# countries nobody plays are played by the server's AI.
#
#   tests/campaign_turnstate_test.sh [build-dir]
#
# WHAT IT PROVES
#
#   1. Every turn, after its delta, each player is sent the state a delta does
#      not carry (NetMsg::TurnState): relations, identities, the rebel states.
#      Before it, a war declared on turn 2 never reached anybody's Relations
#      screen, and rebel states arrived as "Rebel State N" with no flag.
#   2. That state carries no letters -- they are private.
#   3. A war the server declares is in the next state players are sent.
#   4. Rebel states arrive named and flagged.
#   5. A late joiner gets the same state with their snapshot.
#   6. A seated player who SUBMITS is not played by the AI. A dedicated server
#      holds no country, so "skip the player" skipped nobody and the AI took
#      every human's whole turn on top of their orders -- declared their wars,
#      sent their orders. The AI moves a country's research budget on almost
#      every turn it plays, so alice's must never move.
#   7. And a seated player who does NOT submit still is, under "absent: AI":
#      carol's budget must move. Without this the check in 6 could pass on a
#      server whose AI plays nobody at all.
#
# Local only: the stand-in issuer, no account, no network.

set -u
. "$(dirname "$0")/campaign_lib.sh"
campaign_setup "${1:-$campaign_root/build}" campaignturnstate
campaign_config map='"map"' turn-seconds=12 auto.start-at-players=3 auto.start-min-players=3

start_server "$work/server.log" || { bad "the server opens"; tail -20 "$work/server.log"; exit 1; }
ok "the server opens"

turns=6
alog="$work/alice.log" clog="$work/carol.log" blog="$work/bob.log"
client dev-alice --claim-index 0 --submit --submit-after 1 --seconds 400 \
       --until "TURNSTATE turn=$turns" > "$alog" 2>&1 &
a=$!; pids+=("$a")
client dev-carol --claim-index 1 --seconds 400 --until "TURNSTATE turn=$turns" > "$clog" 2>&1 &
c=$!; pids+=("$c")
# Dave never submits either, but has said "leave me idle if I miss a turn".
dlog="$work/dave.log"
client dev-dave --claim-index 2 --away-idle --seconds 400 --until "TURNSTATE turn=$turns" > "$dlog" 2>&1 &
d=$!; pids+=("$d")

# Bob arrives mid-game, as a spectator, once a few turns have been played.
wait_for_line "$alog" '^TURNSTATE turn=3' 2400 "$a"
client dev-bob --seconds 60 --until "TURNSTATE" > "$blog" 2>&1
wait "$a" "$c" "$d" 2>/dev/null

echo "=== every turn, the state with it ==="
deltas="$(grep -c '^DELTA' "$alog")"
# Turn 0's came with the world at the start; every turn after has its own.
states="$(grep -c '^TURNSTATE turn=[1-9]' "$alog")"
check "alice played $turns turns" "[ '$deltas' -ge $turns ]" "deltas=$deltas: $(tail -3 "$alog")"
check "a state followed every turn" "[ '$states' = '$deltas' ]" "deltas=$deltas states=$states"
check "no letters in any state" "! grep '^TURNSTATE' '$alog' '$clog' '$blog' | grep -q 'mail=1'"
check "the map's starting wars came with the world" "grep '^TURNSTATE turn=0 ' '$alog' | grep -qE 'wars=[A-Z]{3}-[A-Z]{3}'" \
      "$(grep '^TURNSTATE turn=0 ' "$alog")"

# A war the server declared during turn k must be in the state for turn k.
python3 - "$work/server.log" "$alog" <<'PY' > "$work/declared.txt"
import re, sys
log, alog = sys.argv[1], sys.argv[2]
turn, found = 1, []
for line in open(log, errors="replace"):
    m = re.search(r"turn (\d+) resolved", line)
    if m: turn = int(m.group(1)) + 1; continue
    m = re.match(r"\[WAR\] (\w+) declares war on (\w+)", line)
    if m: found.append((turn, "-".join(sorted(m.groups()))))
states = {}
for line in open(alog):
    m = re.match(r"TURNSTATE turn=(\d+) .*wars=(\S*) riso=(\S*)", line)
    if m: states[int(m.group(1))] = (set(m.group(2).split(",")), set(m.group(3).split(",")))
# A war on a rebel state that was crushed in the same turn ended with it, and
# rightly is not there: only wars whose rebel side still exists are owed.
def alive(t, w):
    return all(not re.fullmatch(r"R\d+", s) or s in states[t][1] for s in w.split("-"))
checked = [(t, w) for t, w in found if t in states and alive(t, w)]
missing = [(t, w) for t, w in checked if w not in states[t][0]]
print(len(checked), len(missing), " ".join("%d:%s" % x for x in missing[:5]))
PY
read -r nwars nmissing why < "$work/declared.txt"
check "the server declared wars to check against" "[ '$nwars' -gt 0 ]"
check "every war it declared reached the players ($nwars)" "[ '$nmissing' = 0 ]" "missing: $why"

echo "=== rebel states ==="
last="$(grep '^TURNSTATE' "$alog" | tail -1)"
rebels="$(printf '%s' "$last" | sed -n 's/.* rebels=\([0-9]*\).*/\1/p')"
named="$(printf '%s' "$last" | sed -n 's/.* named=\([0-9]*\).*/\1/p')"
if [ "${rebels:-0}" -gt 0 ]; then
    check "every rebel state arrives named and flagged ($rebels)" "[ '$named' = '$rebels' ]" "$last"
else
    echo "  (no rebellion in $turns turns on this seed; naming not checked)"
fi

echo "=== a late joiner ==="
check "bob got the world" "grep -q '^SNAPSHOT' '$blog'" "$(tail -3 "$blog")"
check "and the state with it" "grep -q '^TURNSTATE' '$blog'" "$(tail -3 "$blog")"

echo "=== who the AI plays ==="
amine="$(grep '^TURNSTATE' "$alog" | sed -n 's/.* my=\([0-9]*\).*/\1/p' | sort -u)"
cmine="$(grep '^TURNSTATE' "$clog" | sed -n 's/.* my=\([0-9]*\).*/\1/p' | sort -u)"
check "alice and carol each hold a country" "[ -n '$amine' ] && [ '$amine' != 0 ] && [ -n '$cmine' ] && [ '$cmine' != 0 ]" \
      "alice=$amine carol=$cmine"
aseen="$(grep '^TURNSTATE' "$alog" | sed -n 's/.*\( alloc=[^ ]* active=[^ ]*\).*/\1/p' | sort -u | wc -l | tr -d ' ')"
cseen="$(grep '^TURNSTATE' "$clog" | sed -n 's/.*\( alloc=[^ ]* active=[^ ]*\).*/\1/p' | sort -u | wc -l | tr -d ' ')"
check "alice submitted, so the AI never touched her research" "[ '$aseen' = 1 ]" \
      "$(grep '^TURNSTATE' "$alog" | sed -n 's/.*\( alloc=[^ ]* active=[^ ]*\).*/\1/p' | sort -u | tr '\n' '|')"
check "carol did not, so the AI played her (absent: AI)" "[ '$cseen' -gt 1 ]" \
      "$(grep '^TURNSTATE' "$clog" | sed -n 's/.*\( alloc=[^ ]* active=[^ ]*\).*/\1/p' | sort -u | tr '\n' '|')"
dseen="$(grep '^TURNSTATE' "$dlog" | sed -n 's/.*\( alloc=[^ ]* active=[^ ]*\).*/\1/p' | sort -u | wc -l | tr -d ' ')"
check "dave did not either, but asked to be left idle -- and was" "[ '$dseen' = 1 ]" \
      "$(grep '^TURNSTATE' "$dlog" | sed -n 's/.*\( alloc=[^ ]* active=[^ ]*\).*/\1/p' | sort -u | tr '\n' '|')"

# Money lives on the country record, which no delta carried: a player's own
# treasury sat at its join-time value for the whole game.
moneys="$(grep '^TURNSTATE turn=[1-9]' "$alog" | sed -n 's/.* money=\([^ ]*\).*/\1/p' | sort -u | wc -l | tr -d ' ')"
check "alice's treasury reaches her, and moves with the turns" "[ '$moneys' -gt 1 ]" \
      "$(grep '^TURNSTATE' "$alog" | sed -n 's/.*\( money=[^ ]*\).*/\1/p' | tr '\n' ' ')"

kill -TERM "$server_pid"; wait "$server_pid" 2>/dev/null
echo
if [ "$fail" = 0 ]; then echo "campaign turnstate: all checks passed"; else echo "campaign turnstate: FAILED"; fi
exit "$fail"
