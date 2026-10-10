#!/usr/bin/env bash
# Render's free plan, simulated with Docker, end to end.
#
#   tests/render_sim_test.sh
#
# The real deploy/server image, run the way Render runs it: 512 MB of memory,
# a tenth of one CPU, $PORT set, a disk that starts empty on every start, and a
# NEW instance started before the old one is stopped on a redeploy. Against it:
# the stand-in account service, relay and language model (tests/mock_issuer.mjs),
# a bare git repository standing in for the private campaign repo, and a
# scripted player who joins through the relay with the code alone.
#
# It requires that:
#   1. the health check answers within seconds, before the world has loaded
#      (Render fails a deploy whose port never opens);
#   2. the server opens on Modern Day under 512 MB / 0.1 CPU, as a lobby that
#      does not start by itself;
#   3. a REDEPLOY hands over: the new instance is healthy while it waits, the
#      old one saves and lets go when stopped, and the new one carries on with
#      the same code and the player's country -- without the two ever hosting
#      the campaign at once;
#   4. a CRASH (SIGKILL, no goodbye) is survived once the lock goes stale;
#   5. started by hand, a turn resolves on 0.1 CPU, a letter reaches the model
#      and its reply reaches the player;
#   6. a restart mid-game resumes the game, the turn and the letters;
#   7. the model's API key never reaches the campaign repository;
#   8. memory stays inside the plan throughout, and nothing is OOM-killed.
#
# Needs Docker. Builds the image (several minutes the first time).
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
ok()    { printf '  %-66s ok\n' "$1"; }
bad()   { printf '  %-66s FAILED\n' "$1"; [ -n "${2:-}" ] && printf '      %s\n' "$2"; fail=1; }
check() { if eval "$2"; then ok "$1"; else bad "$1" "${3:-}"; fi; }

id="odsim$$"
net="$id-net"; vol="$id-repo"
img="od-server:sim"; cimg="od-server:sim-build"
peak_file="$(mktemp)"; echo 0 > "$peak_file"
cleanup() {
    [ -n "${sampler:-}" ] && kill "$sampler" 2>/dev/null
    # Each instance's log, kept: a failure here is otherwise unexplainable.
    for c in a b c d issuer; do docker logs "$id-$c" > "/tmp/$id-$c.log" 2>&1 || true; done
    echo "instance logs: /tmp/$id-{a,b,c,d,issuer}.log"
    docker rm -f "$id-issuer" "$id-a" "$id-b" "$id-c" "$id-d" >/dev/null 2>&1
    docker network rm "$net" >/dev/null 2>&1
    docker volume rm "$vol" >/dev/null 2>&1
    rm -f "$peak_file"
}
trap cleanup EXIT

echo "=== building the image ==="
# OD_SIM_REUSE=1 uses images already built -- the build copies the whole tree,
# so touching any file (this one included) means a full rebuild otherwise.
if [ "${OD_SIM_REUSE:-0}" = 1 ] && docker image inspect "$img" "$cimg" >/dev/null 2>&1; then
    echo "  (reusing $img and $cimg)"
else
    "$root/deploy/server/build_local.sh" "$img" >/tmp/$id-build.log 2>&1 \
        || { tail -30 /tmp/$id-build.log; echo "image build failed"; exit 1; }
    "$root/deploy/server/build_local.sh" "$cimg" --target build --build-arg OD_TEST_CLIENT=1 \
        >>/tmp/$id-build.log 2>&1 || { tail -30 /tmp/$id-build.log; echo "client build failed"; exit 1; }
fi
ok "the image builds"
check "and it is not oversized" \
      "[ \$(docker image inspect $img --format '{{.Size}}') -lt 600000000 ]" \
      "$(docker image inspect $img --format '{{.Size}}') bytes"

docker network create "$net" >/dev/null
docker volume create "$vol" >/dev/null
docker run --rm --user 0 --entrypoint sh -v "$vol:/repo" "$img" \
    -c 'git init -q --bare /repo/campaign.git && chown -R 1000:1000 /repo' \
    || { echo "could not make the stand-in repository"; exit 1; }
docker run -d --name "$id-issuer" --network "$net" -v "$root/tests:/t:ro" \
    -e MOCK_LLM_DELAY_MS=4000 \
    --network-alias issuer node:20-slim node /t/mock_issuer.mjs --port 8787 >/dev/null
for _ in $(seq 1 60); do docker logs "$id-issuer" 2>&1 | grep -q "ready on" && break; sleep 1; done

# One instance, as Render would start it. In the stand-in service's network
# namespace, so it is reachable as localhost: the game refuses to send account
# credentials over plain http to anything else, and the real service is https.
# A port each, because a redeploy runs two at once and both own a health port.
port_of() { case "$1" in *-a) echo 10001;; *-b) echo 10002;; *-c) echo 10003;; *) echo 10004;; esac; }
instance() {   # <name>
    docker run -d --name "$1" --network "container:$id-issuer" \
        --memory 512m --memory-swap 512m --cpus 0.1 \
        -v "$vol:/repo" \
        -e PORT="$(port_of "$1")" -e OD_RELAY=1 \
        -e RENDER_SERVICE_NAME=od-tournament -e RENDER_INSTANCE_ID="$1" \
        -e OD_CAMPAIGN_REPO=file:///repo/campaign.git \
        -e OD_ISSUER=http://localhost:8787 -e OD_ACCOUNT_TOKEN=dev-host \
        -e OD_SERVER_NAME="Render rehearsal" -e OD_MAP=map -e OD_TURN_SECONDS=120 \
        -e OD_START_AT_PLAYERS=0 \
        -e OD_LLM_ENDPOINT=http://localhost:8787/v1 -e OD_LLM_MODEL=sim-model \
        -e OD_LLM_KEY=SIM-SECRET-KEY-DO-NOT-PUSH \
        -e OD_SYNC_STALE_SECONDS=90 -e OD_BEAT_SECONDS=20 \
        "$img" >/dev/null
}
health() { docker exec "$1" python3 -c "import urllib.request,sys; r=urllib.request.urlopen('http://127.0.0.1:$(port_of "$1")/healthz', timeout=3); print(r.status, r.read().decode().strip())" 2>/dev/null; }
wait_open() {   # <name> <seconds>
    for _ in $(seq 1 "$2"); do
        docker logs "$1" 2>&1 | grep -q "session open" && return 0
        [ "$(docker inspect -f '{{.State.Running}}' "$1" 2>/dev/null)" = true ] || return 1
        sleep 1
    done
    return 1
}
code_of() { docker logs "$1" 2>&1 | sed -n 's/.*Join code: \([A-Z0-9-]*\).*/\1/p' | tail -1; }
player() {   # <args...>
    docker run --rm --network "container:$id-issuer" "$cimg" /src/build/CampaignClient \
        --issuer http://localhost:8787 --address "" --code "$code" --token dev-alice "$@"
}
sample() {   # peak memory of any instance, every 2 s
    while :; do
        docker stats --no-stream --format '{{.Name}} {{.MemUsage}}' 2>/dev/null |
            awk -v id="$id" '$1 ~ id"-[abcd]$" { v=$2; u=v; gsub(/[0-9.]/,"",u); gsub(/[A-Za-z]/,"",v);
                 m = (u=="GiB") ? v*1024 : (u=="KiB") ? v/1024 : v; if (m>max) max=m } END { print int(max) }' |
            while read -r m; do [ "${m:-0}" -gt "$(cat "$peak_file")" ] && echo "$m" > "$peak_file"; done
        sleep 2
    done
}
sample & sampler=$!

echo "=== first start ==="
t0=$(date +%s)
instance "$id-a"
h=""; for _ in $(seq 1 20); do h="$(health "$id-a")"; [ -n "$h" ] && break; sleep 1; done
check "the health check answers within 20 s, before the world loads" "printf '%s' '$h' | grep -q '^200 ok'" "got: $h"
wait_open "$id-a" 900; opened=$(( $(date +%s) - t0 ))
check "the server opens on Modern Day at 0.1 CPU (${opened}s)" "docker logs '$id-a' 2>&1 | grep -q 'session open'" \
      "$(docker logs "$id-a" 2>&1 | tail -15)"
check "it says the model answers" "docker logs '$id-a' 2>&1 | grep -q 'language model: sim-model'"
code="$(code_of "$id-a")"
check "it has a join code" "[ -n '$code' ]"
for _ in $(seq 1 30); do health "$id-a" | grep -q serving && break; sleep 2; done
check "and the health stage says it is serving" "health '$id-a' | grep -q serving" "$(health "$id-a")"

# Until the claim lands: the first roster line always says no country yet.
player --claim-index 0 --seconds 30 > /tmp/$id-p1.log 2>&1
mine="$(sed -n 's/^ROSTER me=\([1-9][0-9]*\).*/\1/p' /tmp/$id-p1.log | head -1)"
check "a player joins through the relay with the code alone, and claims" "[ -n '$mine' ]" \
      "$(grep -vE '^(LOBBYMAP|COUNTRIES)' /tmp/$id-p1.log | tail -5)"
sleep 15
check "it stays a lobby: nothing starts by itself" "! docker logs '$id-a' 2>&1 | grep -q 'turn 1 resolved'"

echo "=== a redeploy: new instance first, old one stopped once it is healthy ==="
instance "$id-b"
h=""; for _ in $(seq 1 30); do h="$(health "$id-b")"; [ -n "$h" ] && break; sleep 1; done
check "the new instance is healthy while the old one still runs" "printf '%s' '$h' | grep -q '^200 ok'" "got: $h"
for _ in $(seq 1 30); do docker logs "$id-b" 2>&1 | grep -q "waiting for the previous instance" && break; sleep 2; done
check "and it waits instead of hosting the same campaign twice" \
      "docker logs '$id-b' 2>&1 | grep -q 'waiting for the previous instance' && ! docker logs '$id-b' 2>&1 | grep -q 'session open'"
docker stop -t 120 "$id-a" >/dev/null
check "the old instance saved and let go when stopped" \
      "docker logs '$id-a' 2>&1 | grep -q 'campaign_sync: pushed' && docker logs '$id-a' 2>&1 | grep -q 'campaign_sync: released'" \
      "$(docker logs "$id-a" 2>&1 | grep -E 'campaign_sync|stopping|saving' | tail -5)"
wait_open "$id-b" 900
check "the new instance takes over" "docker logs '$id-b' 2>&1 | grep -q 'session open'" "$(docker logs "$id-b" 2>&1 | tail -8)"
check "with the same join code" "[ '$(code_of "$id-b")' = '$code' ]" "was $code, now $(code_of "$id-b")"
player --seconds 60 --until "ROSTER me=$mine " > /tmp/$id-p2.log 2>&1
check "and the player gets their country back" "grep -q '^ROSTER me=$mine ' /tmp/$id-p2.log" \
      "$(grep ROSTER /tmp/$id-p2.log | tail -3)"

echo "=== a crash: killed, no goodbye ==="
docker kill "$id-b" >/dev/null
t1=$(date +%s)
instance "$id-c"
wait_open "$id-c" 1200
check "a new instance takes over once the dead one's lock is stale ($(( $(date +%s) - t1 ))s)" \
      "docker logs '$id-c' 2>&1 | grep -q 'session open'" "$(docker logs "$id-c" 2>&1 | tail -8)"
player --seconds 60 --until "ROSTER me=$mine " > /tmp/$id-p3.log 2>&1
check "the player still has their country" "grep -q '^ROSTER me=$mine ' /tmp/$id-p3.log"

echo "=== the game, started by hand ==="
docker exec "$id-c" sh -c 'echo start >> /state/commands.txt'
player --mail-to-index 7 --mail-body "We propose a trade agreement. What do you say?" \
       --submit --submit-after 1 --seconds 900 --until BOTMAIL > /tmp/$id-p4.log 2>&1
check "a turn resolves on 0.1 CPU" "docker logs '$id-c' 2>&1 | grep -q 'turn 1 resolved'" \
      "$(docker logs "$id-c" 2>&1 | grep -E 'resolved|start' | tail -3)"
check "the letter reached the model, and its reply reached the player" \
      "grep -q '^BOTMAIL ' /tmp/$id-p4.log" "$(grep -E '^(TURN|DELTA|MAIL|BOTMAIL|TIMEOUT)' /tmp/$id-p4.log | tail -6)"
turns="$(docker logs "$id-c" 2>&1 | grep -c 'resolved\.')"
docker logs "$id-c" 2>&1 | grep -E '^\[[0-9:]+ INFO\] turn [0-9]+ resolved' | head -3

echo "=== a restart in the middle of the game ==="
docker stop -t 120 "$id-c" >/dev/null
instance "$id-d"
wait_open "$id-d" 900
check "it comes back into the game, not the lobby" \
      "docker logs '$id-d' 2>&1 | grep -q 'Resumed the campaign at turn'" \
      "$(docker logs "$id-d" 2>&1 | grep -iE 'resum|lobby' | tail -3)"
player --seconds 120 --until BOTMAIL > /tmp/$id-p5.log 2>&1
check "the player is still theirs, with their letters" \
      "grep -q '^ROSTER me=$mine ' /tmp/$id-p5.log && grep -q '^BOTMAIL .*history=1' /tmp/$id-p5.log" \
      "$(grep -E '^(ROSTER|BOTMAIL|MAIL)' /tmp/$id-p5.log | tail -4)"

echo "=== what is in the campaign repository ==="
docker run --rm --entrypoint sh -v "$vol:/repo" "$img" -c \
    'cd /tmp && git clone -q -b campaign /repo/campaign.git c && cd c && ls -1 && ! grep -rq SIM-SECRET-KEY . ; echo key_absent=$?' \
    > /tmp/$id-repo.txt 2>&1
check "it holds the world, the settings and the identity" \
      "grep -q server.json /tmp/$id-repo.txt && grep -q saves /tmp/$id-repo.txt && grep -q account.json /tmp/$id-repo.txt" \
      "$(cat /tmp/$id-repo.txt)"
check "and NOT the model's API key" "grep -q 'key_absent=0' /tmp/$id-repo.txt" "$(cat /tmp/$id-repo.txt)"

echo "=== the plan's limits ==="
peak="$(cat "$peak_file")"
check "peak memory stayed under 512 MB (${peak} MB)" "[ '$peak' -gt 0 ] && [ '$peak' -lt 480 ]"
oom=""; for c in "$id-d"; do oom="$oom$(docker inspect -f '{{.State.OOMKilled}}' "$c" 2>/dev/null)"; done
check "nothing was killed for memory" "! printf '%s' '$oom' | grep -q true"

docker stop -t 120 "$id-d" >/dev/null
echo
echo "first open ${opened}s at 0.1 CPU; ${turns} turn(s) resolved; peak ${peak} MB"
if [ "$fail" = 0 ]; then echo "render sim: all checks passed"; else echo "render sim: FAILED"; fi
exit "$fail"
