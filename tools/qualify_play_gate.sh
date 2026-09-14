#!/usr/bin/env bash
# What did the 5-turn game attempt actually prove?
#
#   tools/qualify_play_gate.sh <sim-log> <exit-code>
#
# Prints one line: "<ok|skip|fail>\t<reason>".
#
# WHY THIS IS ITS OWN FILE
#
# qualify.sh used to decide this inline, and got it wrong in a way that cost a
# release. It asked the log FIRST:
#
#     if grep -q 'suitable pixel format|...OpenGL|...GLFW|GLX' "$sim_log"; then
#         SKIPPED -- this machine has no usable display
#     elif [ "$sim_rc" -ne 0 ]; then
#         FAILED
#
# The display test won unconditionally, so ANY death on a machine that had also
# printed a GL message was excused -- including a real one. On 2026-09-14 the
# macOS runners did exactly that: the game took SIGSEGV, and the step reported
#
#     SKIPPED -- this machine has no usable display
#
# then the run finished "macos QUALIFIED -- built, tested, and played a game",
# which was false in its last four words. A gate that reads a segfault as a
# skip is not a gate, and this is the one step qualify.sh says CI cannot fake.
# The project has already shipped through this hole once: v1.0.3a went out with
# an executable that could not start, on Windows, the platform whose "does it
# run" check was permanently skipped.
#
# WHAT SEPARATES THE TWO CASES
#
# Game::init() calls InitWindow() in EVERY mode, --simulate included (see
# src/server/ServerRaylib.cpp), and runHeadlessSimulation prints its first
# [SIM] line only after that returns. So the marker is decisive:
#
#   no [SIM] at all   the process never got past opening a window, which is
#                     what a machine with no display looks like
#   [SIM] present     the window opened. Whatever happened next is the build's
#                     own doing and is a FAILURE, whatever the log says about GL
#
# Being in its own file is what lets tests/qualify_play_gate_test.sh feed it
# the real logs from that day -- the macOS segfault and the Windows pass -- and
# assert the verdicts. Inline in qualify.sh it was reachable only by running a
# full platform qualification, which is why it went unexamined for so long.
set -uo pipefail

log="${1:-}"
rc="${2:-0}"

if [ -z "$log" ] || [ ! -f "$log" ]; then
    printf 'fail\tno simulation log at %s\n' "${log:-<unset>}"
    exit 0
fi

# Each platform words "there is no GPU here" differently, and the list has to
# carry all of them or a machine that simply cannot draw is called broken:
#
#   macOS    NSGL: Failed to find a suitable pixel format
#   Windows  WGL: The driver does not appear to support OpenGL
#   Linux    GLX / Failed to initialize GLFW  (usually avoided by xvfb)
no_display_re='suitable pixel format|does not appear to support OpenGL|could not open a window|Failed to initialize GLFW|GLX'

started=0
grep -qE '^\[SIM\]' "$log" && started=1

no_display=0
grep -qE "$no_display_re" "$log" && no_display=1

# A signal is worth naming. "exited 139" is a number; "killed by signal 11
# (SIGSEGV)" is a diagnosis, and this gate exists because a crash was once
# filed under the wrong heading.
died_how="exited $rc"
if [ "$rc" -gt 128 ]; then
    sig=$((rc - 128))
    case "$sig" in
        4)  died_how="killed by signal 4 (SIGILL)"  ;;
        6)  died_how="killed by signal 6 (SIGABRT)" ;;
        8)  died_how="killed by signal 8 (SIGFPE)"  ;;
        9)  died_how="killed by signal 9 (SIGKILL)" ;;
        11) died_how="killed by signal 11 (SIGSEGV)" ;;
        *)  died_how="killed by signal $sig" ;;
    esac
fi

if [ "$started" -eq 1 ]; then
    # The window opened. A GL message in the log is now just a message.
    if [ "$rc" -ne 0 ]; then
        printf 'fail\tthe game started simulating and then %s\n' "$died_how"
    else
        printf 'ok\tthe simulation ran to completion\n'
    fi
    exit 0
fi

# Never reached the simulation.
if [ "$no_display" -eq 1 ]; then
    printf 'skip\tthis machine has no usable display, so the game cannot open a window here\n'
else
    printf 'fail\tthe simulation never started and the log names no display problem (%s)\n' "$died_how"
fi
