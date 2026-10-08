#!/usr/bin/env bash
# A server that plays a long campaign without growing, crashing or slowing.
#
#   tests/campaign_soak_test.sh [build-dir] [turns]     (default 120 turns)
#
# A month of daily turns is thirty; two months is sixty. What breaks a server
# over that span is not one turn, it is what each turn LEAVES BEHIND: a list
# that only grows, a snapshot that gets bigger every turn until it no longer
# fits in a frame, a roster that remembers every passer-by. None of those show
# up in a test that plays three turns.
#
# So this plays `turns` turns as fast as the server can resolve them -- two
# players who submit the moment each turn opens, which makes the turn resolve
# immediately -- while strangers drop in to spectate and leave every few
# turns. It samples the server's memory as it goes and fails if:
#
#   - the server stops, or stops advancing;
#   - resident memory keeps climbing (last sample more than 25% and 150 MB over
#     the sample taken once the world was warm), or ever passes 256 MB -- half
#     of what a free container host allows;
#   - the world a late joiner is sent grows past what a relay will carry;
#   - the roster keeps every spectator who ever looked in.
#
# Default 120 turns is four months of a turn a day. Set more to go further.

set -u
. "$(dirname "$0")/campaign_lib.sh"
campaign_setup "${1:-$campaign_root/build}" campaignsoak
turns="${2:-120}"
campaign_config turn-seconds=60 auto.start-at-players=2 auto.start-min-players=2 \
    late-join='"spectate"'

echo "=== $turns turns, played as fast as they resolve ==="
start_server "$work/server.log" || { bad "the server opens a session"; tail -20 "$work/server.log"; exit 1; }
ok "the server opens a session"

client dev-alice --claim-index 0 --submit --seconds 100000 > "$work/alice.log" 2>&1 &
pids+=("$!")
for _ in $(seq 1 100); do grep -q "^ROSTER me=[1-9]" "$work/alice.log" && break; sleep 0.1; done
client dev-bob --claim-index 1 --submit --seconds 100000 > "$work/bob.log" 2>&1 &
pids+=("$!")

samples=()
warm=0
last_turn=0
stall=0
drive_by=0
max_snapshot=0
max_peers=0
started=$(date +%s)
while :; do
    sleep 1
    if ! kill -0 "$server_pid" 2>/dev/null; then
        bad "the server is still running" "it exited at turn $last_turn"
        tail -20 "$work/server.log"
        break
    fi
    t="$(grep -c 'turn [0-9]* resolved' "$work/server.log")"
    if [ "$t" = "$last_turn" ]; then
        stall=$((stall + 1))
        if [ "$stall" -ge 120 ]; then bad "turns keep advancing" "stuck at turn $t for 2 minutes"; break; fi
        continue
    fi
    stall=0
    last_turn="$t"

    # A stranger looks in every 10 turns and leaves again. Non-dev tokens get a
    # fresh pseudonym from the stand-in issuer every time: a new person.
    if [ $((t / 10)) -gt "$drive_by" ]; then
        drive_by=$((t / 10))
        client "visitor-$t" --seconds 30 --until SNAPSHOT > "$work/visitor.log" 2>&1
        b="$(sed -n 's/^SNAPSHOT turn=[0-9]* bytes=\([0-9]*\).*/\1/p' "$work/visitor.log" | tail -1)"
        [ -n "$b" ] && [ "$b" -gt "$max_snapshot" ] && max_snapshot="$b"
        rss="$(rss_mb "$server_pid")"
        samples+=("$t:$rss")
        [ "$warm" = 0 ] && [ "$t" -ge 20 ] && warm="$rss"
        size=$(( $(wc -c < "$save") / 1024 ))
        peers="$(sed -n 's/.*peers=\([0-9]*\).*/\1/p' "$work/bob.log" | tail -1)"
        [ -n "$peers" ] && [ "$peers" -gt "$max_peers" ] && max_peers="$peers"
        printf '    turn %4d  rss %4s MB  save %6d KB  snapshot %7s B  roster %s\n' \
               "$t" "$rss" "$size" "${b:-?}" "${peers:-?}"
    fi
    [ "$t" -ge "$turns" ] && break
done
elapsed=$(( $(date +%s) - started ))

check "it played $turns turns" "[ '$last_turn' -ge '$turns' ]" "reached $last_turn"
# A free container host gives 512 MB. Since the map rasters are held compact
# (ProvinceMap::compact) a 1914 server sits near 100 MB; half the host is the
# line that must never be crossed, so a regression shows here, not as an
# out-of-memory kill in the third week of somebody's tournament.
peak=0
for sample in "${samples[@]}"; do v="${sample#*:}"; [ -n "$v" ] && [ "$v" -gt "$peak" ] && peak="$v"; done
final="$(rss_mb "$server_pid")"

# AN UNMEASURABLE NUMBER IS NOT A PASSING ONE. If rss_mb could not get a figure
# -- which is what happens where neither ps -o rss= nor PowerShell will answer
# for this process -- then `[ '' -le 256 ]` is a syntax error that the check
# would report as a failure about memory, and a default of 0 would report it as
# a triumph. Neither is true, so it is said plainly instead and counted as
# neither pass nor failure.
if [ -z "$final" ] || [ "$peak" = 0 ]; then
    printf '  %-64s %s\n' "resident size is not measurable here" "skip"
    printf '  %-64s %s\n' "  (rss_mb returned nothing; the two memory checks did not run)" ""
else
    check "it never used more than half a 512 MB host (peak ${peak} MB)" "[ '$peak' -le 256 ]"
    check "memory did not keep climbing (warm ${warm} MB, now ${final} MB)" \
          "[ '$warm' -gt 0 ] && [ '$final' -le \$(( warm + warm / 4 + 150 )) ]"
fi
# The relay carries frames up to 8 MB; a world that outgrows it is a campaign
# nobody can join late. Half of that is the line a long game must stay under.
check "a late joiner's world stays well under the relay's 8 MB (max $max_snapshot B)" \
      "[ '$max_snapshot' -gt 0 ] && [ '$max_snapshot' -lt 4194304 ]"
check "the roster did not keep every passer-by (max $max_peers)" \
      "[ '$max_peers' -le 40 ]"
check "no turn was reported unsaved" "! grep -q 'appendTurn failed' '$work/server.log'"
echo "    $last_turn turns in ${elapsed}s"

od_stop_server "$server_pid" "$work/commands.txt"; wait "$server_pid"; rc=$?
check "and it still stops cleanly" "[ '$rc' -eq 0 ]" "exit $rc"

echo
if [ "$fail" = 0 ]; then echo "campaign soak: all checks passed"; else echo "campaign soak: FAILED"; fi
exit "$fail"
