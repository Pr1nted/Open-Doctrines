#!/usr/bin/env bash
# Letters cross the network, and the host's language model answers them.
#
#   tests/campaign_mail_test.sh [build-dir]
#
# WHAT IT PROVES
#
# Mail used to stay on the machine that wrote it, and a dedicated server never
# switched its language model on, so in a hosted game nobody could write to
# anybody. This runs the real OpenDoctrinesServer on the Modern Day map with a
# stand-in model (tests/mock_issuer.mjs answers /v1/chat/completions with one
# fixed sentence) and requires that:
#
#   1. players are told the host's model answers;
#   2. a letter from one player reaches the other, and only after the turn;
#   3. a letter to a country nobody holds reaches the model, and the model's
#      reply comes back to the writer, marked as the model's;
#   4. the model never speaks for a country a PERSON holds -- Bob is written to
#      and must not answer by machine;
#   5. a late joiner's world carries nobody's letters;
#   6. a player who reconnects gets their own correspondence back;
#   7. a model slower than a turn is asked ONCE per letter, not once per turn
#      until it answers -- which piled 45 requests onto one small model.
#
# Local only: the stand-in issuer and model, no account, no network.

set -u
# Slower than a turn here (each resolves in a few seconds once both players
# have submitted), as a model on a CPU is.
export MOCK_LLM_DELAY_MS=20000
. "$(dirname "$0")/campaign_lib.sh"
campaign_setup "${1:-$campaign_root/build}" campaignmail
campaign_config map='"map"' turn-seconds=25 resume-grace-seconds=5 \
    auto.start-at-players=2 auto.start-min-players=2

# The server's own game config, where the language model is switched on.
python3 - "$work/data/config.json" "$issuer" <<'EOF'
import json, sys
p, issuer = sys.argv[1], sys.argv[2]
c = json.load(open(p))
c.update({"llmEnabled": True, "llmEndpoint": issuer + "/v1", "llmModel": "test-model"})
json.dump(c, open(p, "w"))
EOF

echo "=== letters between players, and to the host's model ==="
start_server "$work/server.log" || { bad "the server opens a session"; tail -20 "$work/server.log"; exit 1; }
ok "the server opens a session"
check "the server says its model answers" \
      "grep -q 'language model: test-model' '$work/server.log'" "$(grep -i 'language model' "$work/server.log")"

secret="Alice writes privately to a neighbour nobody plays"
client dev-alice --claim-index 0 --mail-to-index 5 --mail-body "$secret" \
       --submit --submit-after 2 --seconds 180 --until BOTMAIL > "$work/alice.log" 2>&1 &
alice_pid=$!; pids+=("$alice_pid")
for _ in $(seq 1 100); do grep -q "^ROSTER me=[1-9]" "$work/alice.log" && break; sleep 0.1; done
client dev-bob --claim-index 1 --mail-to-index 0 --mail-body "Bob to Alice: hello, neighbour" \
       --submit --submit-after 2 --seconds 240 > "$work/bob.log" 2>&1 &
bob_pid=$!; pids+=("$bob_pid")

wait "$alice_pid"
sleep 3   # anything the model would wrongly have said as Bob arrives with Alice's reply
kill "$bob_pid" 2>/dev/null; wait "$bob_pid" 2>/dev/null

alice_cid="$(sed -n 's/^ROSTER me=\([1-9][0-9]*\).*/\1/p' "$work/alice.log" | head -1)"
bob_cid="$(sed -n 's/^ROSTER me=\([1-9][0-9]*\).*/\1/p' "$work/bob.log" | head -1)"
ai_cid="$(sed -n 's/^MAILSENT to=\([0-9]*\).*/\1/p' "$work/alice.log" | head -1)"
check "both hold a country, and Alice wrote to a third" \
      "[ -n '$alice_cid' ] && [ -n '$bob_cid' ] && [ -n '$ai_cid' ] && [ '$ai_cid' != '$bob_cid' ]" \
      "alice=$alice_cid bob=$bob_cid ai=$ai_cid"
check "players are told the host's model answers" "grep -q '^SESSION llm=1' '$work/alice.log'"
check "Bob's letter reaches Alice" \
      "grep -q '^MAIL from=$bob_cid to=$alice_cid history=0 status=1 body=Bob to Alice' '$work/alice.log'" \
      "$(grep '^MAIL' "$work/alice.log")"
check "and not before the turn it was posted on" \
      "awk '/^DELTA/{d=1} /^MAIL from=$bob_cid/{exit !d}' '$work/alice.log'"
curl -s "$issuer/llm-stats" > "$work/llm-stats.json"
check "Alice's letter reached the host's model" \
      "grep -q 'Alice writes privately' '$work/llm-stats.json'" "$(head -c 300 "$work/llm-stats.json")"
check "one letter, one request, however many turns it took" \
      "python3 -c \"import json,sys; sys.exit(json.load(open('$work/llm-stats.json'))['requests'] != 1)\"" \
      "$(python3 -c "import json; print('requests:', json.load(open('$work/llm-stats.json'))['requests'])")"
check "and its reply came back to her, as the model's" \
      "grep -q '^BOTMAIL from=$ai_cid to=$alice_cid history=0 status=1 body=Our ministry' '$work/alice.log'" \
      "$(grep -E '^(BOT)?MAIL' "$work/alice.log")"
check "the model never answered as Bob, whom a person plays" \
      "! grep -q '^BOTMAIL from=$alice_cid' '$work/bob.log' && ! grep -q '^BOTMAIL from=$bob_cid' '$work/alice.log'" \
      "$(grep BOTMAIL "$work/bob.log" "$work/alice.log")"
check "nor as Alice, whom Bob wrote to" "! grep -q '^BOTMAIL' '$work/bob.log'" "$(grep BOTMAIL "$work/bob.log")"

echo "=== who else sees it ==="
client dev-carol --secret "$secret" --seconds 30 --until SNAPSHOT > "$work/carol.log" 2>&1
check "a late joiner is sent the world" "grep -q '^SNAPSHOT' '$work/carol.log'" "$(tail -3 "$work/carol.log")"
check "and nobody's letters in it" "grep -q '^SNAPSHOT .* secret=0' '$work/carol.log'" \
      "$(grep SNAPSHOT "$work/carol.log")"
check "nor any letter of anybody's" "! grep -qE '^(BOT)?MAIL' '$work/carol.log'"

client dev-alice --seconds 30 --until BOTMAIL > "$work/alice2.log" 2>&1
check "Alice reconnecting gets her correspondence back" \
      "grep -q '^MAILRESET' '$work/alice2.log' && grep -q '^MAIL from=$alice_cid to=$ai_cid history=1 .*body=$secret' '$work/alice2.log'" \
      "$(grep -E 'MAIL' "$work/alice2.log")"
check "with the model's reply in it" \
      "grep -q '^BOTMAIL from=$ai_cid to=$alice_cid history=1' '$work/alice2.log'"

kill -TERM "$server_pid"; wait "$server_pid"
echo
if [ "$fail" = 0 ]; then echo "campaign mail: all checks passed"; else echo "campaign mail: FAILED"; fi
exit "$fail"
