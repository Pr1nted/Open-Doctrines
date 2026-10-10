#!/usr/bin/env bash
# Does a real hosted model answer the game's letters? (Groq by default.)
#
#   copy the API key, then:   tests/llm_endpoint_live_test.sh --from-clipboard
#   or:                       OD_LLM_KEY=... tests/llm_endpoint_live_test.sh
#
#   OD_LLM_ENDPOINT   default https://api.groq.com/openai/v1
#   OD_LLM_MODEL      default llama-3.1-8b-instant
#
# Runs the real OpenDoctrinesServer on Modern Day with that model behind it,
# a scripted player writes to a country nobody holds, and the model's reply has
# to come back -- the same requests, tools and all, that a server on Render
# makes. The key is read from the environment or the clipboard and is never
# printed; the server's log is checked for it too, so a key that would end up
# in a log file fails the test.
#
# Needs the internet and the key; NOT in run_all.sh. Everything else is local:
# the stand-in account service (tests/mock_issuer.mjs).
set -u
key="${OD_LLM_KEY:-}"
if [ "${1:-}" = "--from-clipboard" ]; then
    key="$(pbpaste 2>/dev/null || xclip -o -selection clipboard 2>/dev/null || true)"
    key="$(printf '%s' "$key" | tr -d '[:space:]')"
fi
[ -n "$key" ] || { echo "no key: copy it and pass --from-clipboard, or set OD_LLM_KEY"; exit 2; }
export OD_LLM_KEY="$key"
endpoint="${OD_LLM_ENDPOINT:-https://api.groq.com/openai/v1}"
model="${OD_LLM_MODEL:-llama-3.1-8b-instant}"
echo "endpoint $endpoint, model $model, key of ${#key} characters (not shown)"

. "$(dirname "$0")/campaign_lib.sh"
campaign_setup "$campaign_root/build" llmlive
campaign_config map='"map"' turn-seconds=30 auto.start-at-players=1 auto.start-min-players=1
python3 - "$work/data/config.json" "$endpoint" "$model" <<'EOF'
import json, os, sys
p, endpoint, model = sys.argv[1:4]
c = json.load(open(p))
c.update({"llmEnabled": True, "llmEndpoint": endpoint, "llmModel": model,
          "llmApiKey": os.environ["OD_LLM_KEY"]})
json.dump(c, open(p, "w"))
EOF

echo "=== a letter to the model, through the real server ==="
start_server "$work/server.log" || { bad "the server opens"; tail -20 "$work/server.log"; exit 1; }
ok "the server opens with the model configured"
client dev-alice --claim-index 0 --mail-to-index 7 \
       --mail-body "We propose a trade agreement between our two countries. What do you say?" \
       --submit --submit-after 1 --seconds 300 --until BOTMAIL > "$work/alice.log" 2>&1
reply="$(sed -n 's/^BOTMAIL .*body=//p' "$work/alice.log" | head -1)"
check "the model answered, and the reply reached the player" "[ -n \"\$reply\" ]" \
      "$(grep -E '\[LLM|\[ADVISOR|language model|HTTP [0-9]{3}|refused' "$work/server.log" | tail -6)"
[ -n "$reply" ] && printf '      reply: %s\n' "$reply"
check "the key is nowhere in the server's log" "! grep -qF \"\$OD_LLM_KEY\" '$work/server.log'"
check "nor in the player's" "! grep -qF \"\$OD_LLM_KEY\" '$work/alice.log'"
kill -TERM "$server_pid"; wait "$server_pid" 2>/dev/null
# The config file holds it by design (as on Render); this sandbox is deleted.
rm -rf "$work"
echo
if [ "$fail" = 0 ]; then echo "llm endpoint live: all checks passed"; else echo "llm endpoint live: FAILED"; fi
exit "$fail"
