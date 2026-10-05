#!/usr/bin/env bash
# The dedicated server plays the same game, turn for turn, given the same seed.
#
#   tests/campaign_det_test.sh <build-dir> <turns> <out-file> [server-binary]
#
# Two scripted players take a country each and submit empty orders the moment
# every turn opens, so the AI plays the rest of the world and every turn
# resolves as soon as it can. With OD_WORLD_SEED pinned and OD_DET_TRACE on,
# the server prints a fingerprint of each turn -- who owns what, every army,
# every treasury -- and those lines are written to <out-file>.
#
# Run it with two binaries and diff the files: an optimisation that changes no
# rule produces identical fingerprints, and one that does shows the first turn
# it changed. That is how the server's memory work was checked.
set -u
. "$(dirname "$0")/campaign_lib.sh"
campaign_setup "${1:-$campaign_root/build}" campaigndet
turns="${2:-40}"; out="${3:?out file}"
[ -n "${4:-}" ] && srv="$4"
campaign_config turn-seconds=600 auto.start-at-players=2 auto.start-min-players=2
export OD_WORLD_SEED=4242 OD_DET_TRACE=1
start_server "$work/server.log" || { tail -20 "$work/server.log"; exit 1; }
client dev-alice --claim 0 --claim-index 0 --submit --seconds 100000 > "$work/alice.log" 2>&1 &
pids+=("$!")
for _ in $(seq 1 100); do grep -q "^ROSTER me=[1-9]" "$work/alice.log" && break; sleep 0.1; done
client dev-bob --claim-index 1 --submit --seconds 100000 > "$work/bob.log" 2>&1 &
pids+=("$!")
for _ in $(seq 1 $((turns * 40))); do
    [ "$(grep -c '^\[DET\]' "$work/server.log")" -ge "$turns" ] && break
    kill -0 "$server_pid" 2>/dev/null || break
    sleep 0.5
done
echo "peak rss: $(rss_mb "$server_pid") MB (now)"
grep '^\[DET\]' "$work/server.log" | head -n "$turns" > "$out"
echo "$(wc -l < "$out") turns fingerprinted -> $out"
