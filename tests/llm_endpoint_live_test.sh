#!/usr/bin/env bash
# Does a real hosted model answer the game's letters? (Groq by default.)
#
#   copy the API key, then:   tests/llm_endpoint_live_test.sh --from-clipboard
#   which models it may use:  tests/llm_endpoint_live_test.sh --from-clipboard --list-models
#   or:                       OD_LLM_KEY=... tests/llm_endpoint_live_test.sh
#
#   OD_LLM_ENDPOINT   default https://api.groq.com/openai/v1
#   OD_LLM_MODEL      default openai/gpt-oss-20b
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
# --list-models (after --from-clipboard): what this key may use, names only.
# Hosted APIs retire models -- llama-3.1-8b-instant went -- so this is how a
# new name is chosen.
if [ "${2:-}" = "--list-models" ]; then
    curl -s "$endpoint/models" -H "Authorization: Bearer $OD_LLM_KEY" |
        python3 -c "import json,sys; d=json.load(sys.stdin); print('\n'.join(sorted(m['id'] for m in d.get('data',[]))) or d)"
    exit 0
fi
model="${OD_LLM_MODEL:-openai/gpt-oss-20b}"
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
       --submit --submit-after 1 --seconds 600 --until BOTMAIL > "$work/alice.log" 2>&1
reply="$(sed -n 's/^BOTMAIL .*body=//p' "$work/alice.log" | head -1)"
check "the model answered, and the reply reached the player" "[ -n \"\$reply\" ]" \
      "$(grep -E '\[LLM|\[ADVISOR|language model|HTTP [0-9]{3}|refused' "$work/server.log" | tail -6)"
[ -n "$reply" ] && printf '      reply: %s\n' "$reply"
check "the key is nowhere in the server's log" "! grep -qF \"\$OD_LLM_KEY\" '$work/server.log'"
check "nor in the player's" "! grep -qF \"\$OD_LLM_KEY\" '$work/alice.log'"
grep -E '^\[LLM\]' "$work/server.log" | sed 's/^/      /' | head -5
kill -TERM "$server_pid"; wait "$server_pid" 2>/dev/null
# The config file holds the key by design (as on Render), so it goes. The logs
# stay, for a failure to be explained -- scrubbed of the key first, though the
# check above already says it is not in them.
rm -f "$work/data/config.json"
python3 - "$work" <<'PY'
import os, sys
key = os.environ["OD_LLM_KEY"]
for root, _, files in os.walk(sys.argv[1]):
    for f in files:
        if f.endswith(".log"):
            p = os.path.join(root, f)
            s = open(p, errors="replace").read()
            if key in s:
                open(p, "w").write(s.replace(key, "***"))
PY
echo "logs kept in $work (no key in them)"
echo
if [ "$fail" = 0 ]; then echo "llm endpoint live: all checks passed"; else echo "llm endpoint live: FAILED"; fi
exit "$fail"
