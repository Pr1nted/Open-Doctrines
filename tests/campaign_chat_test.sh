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

    local alog="$work/alice-$label.log" blog="$work/bob-$label.log"
    # --seconds is a CEILING, not a schedule: the round below ends when the
    # conversation has happened, so a generous one costs nothing on a healthy
    # run and keeps a slow relay handshake from eating the talking time.
    # --chat-after-peers 2: NEITHER SPEAKS UNTIL BOTH ARE IN. A lobby line is
    # live and is never replayed to a late joiner, so "every line each of them
    # says reaches both" is not a property that exists until both have joined.
    # Without it the first to connect races the second, and on macOS CI bob won:
    # he said "bob one" into a session alice was still joining, alice counted
    # five lines instead of six, and the failure read as a dropped message
    # rather than as a question asked too early.
    "$cli" --issuer "$issuer" --address "$addr" --code TEST-GAME --token dev-alice \
           --chat "alice one" --chat "alice two" --chat "alice three" --chat "alice four" \
           --chat-after-peers 2 --seconds 120 > "$alog" 2>&1 &
    local a=$!; pids+=("$a")
    "$cli" --issuer "$issuer" --address "$addr" --code TEST-GAME --token dev-bob \
           --chat "bob one" --chat "bob two" \
           --chat-after-peers 2 --seconds 120 > "$blog" 2>&1 &
    local b=$!; pids+=("$b")

    # THE OPERATOR SPEAKS TO WHOEVER IS CONNECTED. `say` sends a lobby line
    # live and there is no replay of it for a player who was still joining, so
    # saying it on a timer is a bet on how long a handshake takes. Four seconds
    # won that bet for a direct connection on every runner and lost it for a
    # relayed one on Windows -- which is the whole of why "alice hears the
    # operator (relay)" had been red on windows-x64 and windows-arm64 since
    # 2026-10-05, with the relay itself working fine: bob's lines reached alice
    # through it in the same round. Their own first line is the proof that they
    # are in, because campaign_client chats only once the host has given it a
    # peer id.
    local joined=0
    wait_for_line "$alog" '^SAID alice one' 300 "$a" &&
        wait_for_line "$blog" '^SAID bob one' 300 "$b" && joined=1
    check "both players are in before the operator speaks ($label)" "[ $joined = 1 ]" \
          "alice: $(tail -1 "$alog" 2>/dev/null) | bob: $(tail -1 "$blog" 2>/dev/null)"
    printf 'say from the operator\n' >> "$work/commands.txt"

    # Then wait for the last thing each of them should hear rather than for the
    # clock. A ceiling reached here is not reported: the checks below say what
    # is missing, and they say it about the line rather than about the wait.
    for log in "$alog" "$blog"; do
        wait_for_line "$log" '^CHAT from=[0-9]+ alice four' 300
        wait_for_line "$log" '^CHAT from=[0-9]+ from the operator' 300
    done
    kill -TERM "$a" "$b" 2>/dev/null
    wait "$a" "$b" 2>/dev/null

    # Said at all, before asking whether it was heard. Without this, "nobody
    # hears the operator" is two different failures wearing one face: a line
    # the server refused to send (`say` warns and returns when the session is
    # not open) and a line the players missed. The next person to see this red
    # should not have to tell them apart from scratch.
    check "the server said it ($label)" \
          "grep -q '^\[Server\] from the operator' '$work/server-$label.log'" \
          "$(grep -iE 'operator|nobody to say that' "$work/server-$label.log" | tail -3)"

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
