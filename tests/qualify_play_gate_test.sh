#!/usr/bin/env bash
# The release gate's own gate.
#
# tools/qualify_play_gate.sh decides whether the 5-turn game attempt proved the
# build runs, could not be tried, or crashed. It got that wrong once and the
# run still printed "macos QUALIFIED -- built, tested, and played a game", so
# the distinction is checked here rather than trusted.
#
# The first two cases are transcripts of the real runs of 2026-09-14: the macOS
# segfault that was reported as SKIPPED, and the Windows pass beside it.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
gate="$here/../tools/qualify_play_gate.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

checks=0
failed=0

# want <expected-verdict> <exit-code> <name> <<< log text
want() {
    local expect="$1" rc="$2" name="$3"
    local log="$tmp/sim.log"
    cat > "$log"
    checks=$((checks + 1))
    local got verdict
    got="$("$gate" "$log" "$rc")"
    verdict="${got%%	*}"
    if [ "$verdict" = "$expect" ]; then
        printf '  ok    %s\n' "$name"
    else
        failed=$((failed + 1))
        printf '  FAIL  %s\n        wanted %s, got %s (%s)\n' \
               "$name" "$expect" "$verdict" "${got#*	}"
    fi
}

printf 'qualify play gate\n'

# ── THE BUG. A real macOS run: GL refused, the process took SIGSEGV. ──
# The old logic matched the pixel-format line and called this a skip.
want fail 139 "a segfault is a failure even when the log mentions GL" <<'LOG'
INFO: Platform backend: DESKTOP (GLFW)
WARNING: GLFW: Error: 65545 Description: NSGL: Failed to find a suitable pixel format
[SIM] data/STDmaps/1939.odmap — 5 turns
LOG

# ── the genuine skip: no display, and the game never got as far as playing ──
want skip 139 "no display and no simulation is still a skip" <<'LOG'
INFO: Platform backend: DESKTOP (GLFW)
WARNING: GLFW: Error: 65545 Description: NSGL: Failed to find a suitable pixel format
LOG

# ── the real Windows pass from the same day ──
want ok 0 "a completed simulation passes" <<'LOG'
[SIM] D:/a/Open-Doctrines/Open-Doctrines/data/STDmaps/1939.odmap — 5 turns
[SIM] save: build/qualify-sim/saves/Qualify.odsv
[SIM] turn 5/5  (7.5s, 1.51 s/turn)
LOG

# ── a crash with a clean-looking log is not excused either ──
want fail 139 "a crash with no GL message is a failure" <<'LOG'
INFO: Platform backend: DESKTOP (GLFW)
LOG

# ── every platform's wording for "there is no GPU here" ──
want skip 1 "Windows WGL wording is recognised" <<'LOG'
WARNING: WGL: The driver does not appear to support OpenGL
LOG
want skip 1 "Linux GLFW wording is recognised" <<'LOG'
WARNING: Failed to initialize GLFW
LOG

# ── the quiet failure: exited 0 without ever simulating ──
want fail 0 "a clean exit that never simulated is a failure" <<'LOG'
INFO: Platform backend: DESKTOP (GLFW)
LOG

# ── a load failure after the window opened is the build's own problem ──
want fail 1 "a failed map load is a failure, not a skip" <<'LOG'
[SIM] data/STDmaps/1939.odmap — 5 turns
[SIM] could not load data/STDmaps/1939.odmap
LOG

# ── a missing log cannot be a pass ──
checks=$((checks + 1))
if [ "$("$gate" "$tmp/nope.log" 0 | cut -f1)" = "fail" ]; then
    printf '  ok    a missing log is a failure\n'
else
    failed=$((failed + 1)); printf '  FAIL  a missing log is a failure\n'
fi

# ── the signal is NAMED, because "exited 139" is what hid this for so long ──
checks=$((checks + 1))
printf '[SIM] x — 5 turns\n' > "$tmp/sim.log"
if "$gate" "$tmp/sim.log" 139 | grep -q 'SIGSEGV'; then
    printf '  ok    a segfault is reported as SIGSEGV, not as a number\n'
else
    failed=$((failed + 1)); printf '  FAIL  a segfault is reported as SIGSEGV\n'
fi

printf '%d check(s), %d failed\n' "$checks" "$failed"
[ "$failed" -eq 0 ]
