#!/usr/bin/env bash
# Follow one or more logs until a marker appears, and ALWAYS take the tail down.
#
# ── THE LEAK THIS EXISTS TO STOP ──
#
# The pattern typed by hand for months was:
#
#     tail -f a.log b.log | grep -E "..." &
#     p=$!
#     until grep -q DONE a.log; do sleep 20; done
#     kill $p
#
# The `kill` only runs if the shell REACHES it. It does not when the shell is
# killed first -- which is what happens every time a tool call times out, or a
# run is interrupted, or the marker never arrives. The tail is then reparented
# to launchd and follows a finished log for ever.
#
# One at a time is invisible. This machine had 155 of them, each holding open
# file handles for a run that ended hours or days earlier.
#
# A trap fixes it because it fires on the ways out that `kill` at the bottom
# cannot cover: a signal, an error under `set -e`, or any early return.
#
# ── USAGE ──
#
#     tools/watchlog.sh --until DONE_MARKER [--timeout 7200] \
#                       [--match 'REGEX'] log [log...]
#
# Exits 0 when the marker appears in any of the logs, 1 on timeout.

set -uo pipefail

marker=""
timeout=7200
match=""
logs=()

while [ $# -gt 0 ]; do
    case "$1" in
        --until)   marker="${2:-}"; shift 2 ;;
        --timeout) timeout="${2:-7200}"; shift 2 ;;
        --match)   match="${2:-}"; shift 2 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *)         logs+=("$1"); shift ;;
    esac
done

if [ -z "$marker" ] || [ ${#logs[@]} -eq 0 ]; then
    echo "usage: $0 --until MARKER [--timeout SECONDS] [--match REGEX] log [log...]" >&2
    exit 2
fi

# The tail is started AFTER the trap is armed, so there is no window in which a
# signal could arrive with a child running and no handler to clean it up.
tail_pid=""
cleanup() {
    # `kill 0` would take the whole process group including the caller; this
    # kills exactly the child that was started, and only if it is still there.
    [ -n "$tail_pid" ] && kill "$tail_pid" 2>/dev/null
    return 0
}
# ── TWO TRAPS, AND THE DIFFERENCE MATTERS ──
#
# A handler that merely RETURNS does not end the script: bash runs it and then
# carries on where it left off. Trapping TERM with `cleanup` alone therefore
# killed the tail and left this loop spinning to its timeout with nothing to
# watch -- which the test caught, because the wrapper was still alive after the
# tail had gone. Signals must exit explicitly; EXIT does the tidying either way.
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM HUP

# Wait for the files rather than failing on a chain that has not created its log
# yet -- the commonest reason the old pattern printed nothing at all.
waited=0
for f in "${logs[@]}"; do
    while [ ! -f "$f" ] && [ "$waited" -lt "$timeout" ]; do
        sleep 1
        waited=$((waited + 1))
    done
done

if [ -n "$match" ]; then
    tail -n +1 -f "${logs[@]}" | grep --line-buffered -E "$match" &
else
    tail -n +1 -f "${logs[@]}" &
fi
tail_pid=$!

elapsed=0
while [ "$elapsed" -lt "$timeout" ]; do
    for f in "${logs[@]}"; do
        if [ -f "$f" ] && grep -q "$marker" "$f" 2>/dev/null; then
            sleep 2          # let the tail flush the last lines through
            exit 0           # the trap takes the tail down
        fi
    done
    # ── SLEEP IN THE BACKGROUND, THEN WAIT ──
    #
    # Bash will not run a trap while a FOREGROUND child is running: a plain
    # `sleep 5` defers the handler for up to five seconds, so a killed watcher
    # appears to leak its tail for that long. `wait` is interruptible, so the
    # signal is acted on at once.
    sleep 5 &
    wait $! 2>/dev/null
    elapsed=$((elapsed + 5))
done

echo "watchlog: '$marker' did not appear within ${timeout}s" >&2
exit 1
