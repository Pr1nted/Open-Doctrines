#!/usr/bin/env bash
# Lobby chat reaches everyone, in order, directly and through the relay.
#
#   tests/campaign_chat_test.sh [build-dir]
#
# Runs the real OpenDoctrinesServer in a lobby with two scripted players and
# requires that every line each of them says -- and every line the operator
# says with `say` -- reaches BOTH players, in the order it was said, attributed
# to whoever said it -- at a pace inside the host's rate limit, which
# tests/net_chat_test.cpp covers -- and that the server's log shows the
# conversation, since a dedicated server has no chat panel. Then the same over
# the relay.
#
# Local only: the stand-in issuer and relay (tests/mock_issuer.mjs).

set -u
. "$(dirname "$0")/campaign_lib.sh"
campaign_setup "${1:-$campaign_root/build}" campaignchat

run_round() {   # <label> <address-or-empty>
    local label="$1" addr="$2"
    echo "=== $label ==="
    : > "$work/commands.txt"
    start_server "$work/server-$label.log" || { bad "the server opens ($label)"; tail -20 "$work/server-$label.log"; return; }
    ok "the server opens ($label)"

    "$cli" --issuer "$issuer" --address "$addr" --code TEST-GAME --token dev-alice \
           --chat "alice one" --chat "alice two" --chat "alice three" --chat "alice four" \
           --seconds 30 > "$work/alice-$label.log" 2>&1 &
    local a=$!; pids+=("$a")
    "$cli" --issuer "$issuer" --address "$addr" --code TEST-GAME --token dev-bob \
           --chat "bob one" --chat "bob two" --seconds 30 > "$work/bob-$label.log" 2>&1 &
    local b=$!; pids+=("$b")
    sleep 4
    printf 'say from the operator\n' >> "$work/commands.txt"
    wait "$a" "$b" 2>/dev/null

    for who in alice bob; do
        local log="$work/$who-$label.log"
        local got; got="$(grep '^CHAT from=' "$log" | grep -cv 'from the operator')"
        check "$who receives all six player lines ($label)" "[ '$got' = 6 ]" \
              "got $got: $(grep '^CHAT' "$log" | tr '\n' '|')"
        check "$who receives alice's lines in order ($label)" \
              "grep '^CHAT' '$log' | grep -o 'alice [a-z]*' | tr '\n' ' ' | grep -q 'alice one alice two alice three alice four'"
        check "$who hears the operator ($label)" "grep -q '^CHAT from=[0-9]* from the operator' '$log'" \
              "$(grep '^CHAT' "$log" | tail -3)"
        check "nobody is told to slow down at this pace ($label)" "! grep -q 'too quickly' '$log'"
    done
    check "the server's log shows the conversation ($label)" \
          "grep -q '^\[chat\] .*: alice four' '$work/server-$label.log' && grep -q '^\[chat\] .*: bob two' '$work/server-$label.log'" \
          "$(grep '^\[chat\]' "$work/server-$label.log" | head -3)"

    kill -TERM "$server_pid"; wait "$server_pid" 2>/dev/null
}

run_round direct "127.0.0.1:$port"
campaign_config relay=true
run_round relay ""

echo
if [ "$fail" = 0 ]; then echo "campaign chat: all checks passed"; else echo "campaign chat: FAILED"; fi
exit "$fail"
