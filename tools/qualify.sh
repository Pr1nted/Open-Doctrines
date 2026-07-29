#!/usr/bin/env bash
# Qualify a platform: build it, test it, and play a game on it without a person.
#
#   tools/qualify.sh [build-dir]
#
# WHAT "QUALIFIED" MEANS, AND WHY CI DOES NOT ESTABLISH IT
#
# .github/workflows/release-game.yml builds four platforms and packages them.
# It does not run a single test on any of them. "It compiled" and "it works"
# are different claims, and the gap between them is where every one of the web
# build's four breakages lived: each compiled its own translation unit fine and
# died at link or at startup.
#
# So this is the other half. On the machine you are qualifying, in order:
#
#   1. the dependencies raylib needs, installed
#   2. the game and every test target, built
#   3. tests/run_all.sh -- the whole suite
#   4. the four-player multiplayer check, headless
#   5. a real game: load a shipped scenario, resolve turns, write a save
#
# Step 5 is the one CI cannot fake. A build that passes unit tests and cannot
# load a map is a build that does not run, and nothing before step 5 notices.
#
# HEADLESS
#
# Steps 4 and 5 open a window, because the game has one renderer and it is not
# optional. On a VM with no display that is xvfb's job, and this finds it
# rather than failing with an X error that mentions neither X nor the display.
set -uo pipefail

root="${OD_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
build="${1:-$root/build}"

# The repo is found relative to this script, so running a COPY of it silently
# points every step at the wrong tree -- and the first symptom is cmake saying
# `The source directory "/" does not appear to contain CMakeLists.txt`, which
# names neither the script nor the copy. Fail here instead, where the cause is.
if [ ! -f "$root/CMakeLists.txt" ]; then
    echo "qualify.sh: '$root' is not the OpenDoctrines source tree." >&2
    echo "  This script locates the repo relative to itself, so run it in place" >&2
    echo "  (tools/qualify.sh) rather than from a copy, or set OD_ROOT." >&2
    exit 1
fi
fail=0
declare -a failed_steps=()

step()  { printf '\n\033[1m=== %s ===\033[0m\n' "$1"; }
note()  { printf '  %s\n' "$1"; }
bad()   { fail=1; failed_steps+=("$1"); printf '  \033[31mFAILED: %s\033[0m\n' "$1"; }

run_step() {
    local name="$1"; shift
    step "$name"
    if "$@"; then note "ok"; else bad "$name"; fi
}

# ---------------------------------------------------------------- platform ---
# Windows is qualified under Git Bash, not PowerShell, and deliberately: the
# test suite is bash already (tests/run_all.sh, tests/connectivity_test.sh), so
# a PowerShell port of this script would be a second implementation that drifts
# from the one everyone else runs. Git for Windows ships the shell, and you
# need git on the machine regardless.
os="$(uname -s)"
case "$os" in
    Linux)             platform="linux" ;;
    Darwin)            platform="macos" ;;
    MINGW*|MSYS*|CYGWIN*) platform="windows" ;;
    *)                 platform="$os" ;;
esac
note "platform: $platform ($(uname -m))"

# ------------------------------------------------------------ dependencies ---
step "dependencies"
# Root in a container has no sudo and needs none; a user on a desktop has sudo
# and needs it. Resolving it here rather than hardcoding either means the same
# script qualifies a VM and a docker image.
SUDO=""
if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null; then SUDO="sudo"; fi

if [ "$platform" = "linux" ]; then
    if command -v apt-get >/dev/null; then
        # The same list the CI workflow installs, plus what the tests need:
        # node for the stand-in account service, python3 + Pillow for the GIF
        # decode check, and xvfb for the two steps that open a window.
        DEBIAN_FRONTEND=noninteractive $SUDO apt-get update -qq
        DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y -qq \
            build-essential cmake git pkg-config ca-certificates curl gnupg \
            libasound2-dev libx11-dev libxrandr-dev libxi-dev \
            libgl1-mesa-dev libglu1-mesa-dev libxcursor-dev \
            libxinerama-dev libwayland-dev libxkbcommon-dev \
            python3 python3-pip xvfb \
            || bad "installing packages"
        # nodejs and npm are deliberately NOT in that list. Ubuntu's are 12,
        # which is too old for the stand-in account service, and installing
        # them first makes the upgrade WORSE rather than merely pointless:
        # apt's npm drags in libnode-dev, and NodeSource's nodejs package then
        # fails to unpack because both own /usr/include/node/common.gypi.
        # Install the right one once, below, instead of fighting the wrong one.
        python3 -m pip install --quiet --break-system-packages Pillow 2>/dev/null \
            || python3 -m pip install --quiet Pillow 2>/dev/null \
            || note "Pillow not installed; the GIF decode check will be skipped"
    else
        note "no apt-get here -- install the raylib X11/ALSA dev packages yourself"
    fi
else
    note "assuming a working toolchain (cmake, a C++20 compiler, node, python3)"
fi

# Node, at a version that can actually run the stand-in account service.
#
# Checked before the build rather than after, because finding out at the test
# step that this machine cannot run half the suite wastes the ten minutes in
# between. Two separate ways to be wrong are handled here, and the second is
# the one that cost real time:
#
#   absent   -- nothing installed it, because Ubuntu's package is deliberately
#               not in the apt list above
#   too old  -- something else already put node 12 on the machine
#
# Either way the answer is the same: install a current one from NodeSource.
node_major() { node -p 'process.versions.node.split(".")[0]' 2>/dev/null || echo 0; }
if [ "$(node_major)" -lt 18 ]; then
    if command -v node >/dev/null; then
        note "node $(node --version) is too old (need 18+); installing a current one"
    else
        note "node is not installed; installing a current one"
    fi
    if command -v curl >/dev/null && command -v apt-get >/dev/null; then
        # NOT `| $SUDO -E bash -`. As root SUDO is empty, so that expands to
        # `| -E bash -` and the shell tries to run a command called "-E",
        # which exits instantly and leaves curl reporting the symptom
        # ("Failed writing body") rather than the cause. An empty variable in
        # command position is only ever a trap; spell out both cases.
        if [ -n "$SUDO" ]; then
            curl -fsSL https://deb.nodesource.com/setup_20.x | $SUDO -E bash -
        else
            curl -fsSL https://deb.nodesource.com/setup_20.x | bash -
        fi
        DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y -qq nodejs
        note "node is now $(node --version 2>/dev/null || echo 'still missing')"
    fi
    [ "$(node_major)" -ge 18 ] || \
        bad "node 18+ (the stand-in account service needs WebCrypto Ed25519)"
fi

for tool in cmake node python3; do
    command -v "$tool" >/dev/null || bad "$tool is not installed"
done
[ "$fail" -eq 0 ] || { printf '\nCannot qualify without the tools above.\n'; exit 1; }

# A display for the steps that need one. Under xvfb-run the whole rest of the
# script runs inside one server rather than starting one per invocation.
runner=()
if [ "$platform" = "linux" ] && [ -z "${DISPLAY:-}" ]; then
    if command -v xvfb-run >/dev/null; then
        note "no DISPLAY; window steps will run under xvfb-run"
        runner=(xvfb-run -a)
    else
        note "no DISPLAY and no xvfb-run: the window steps will fail"
    fi
fi

# ------------------------------------------------------------------ build ---
# OD_JOBS exists because this project is memory-hungry to compile and the core
# count is a bad proxy for how many of these translation units fit in RAM at
# once. A container given 8 GB and eight cores gets its build OOM-killed, which
# looks exactly like a compiler crash.
jobs="${OD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
note "building with -j$jobs (override with OD_JOBS)"

run_step "configure" cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release
run_step "build the game" cmake --build "$build" --config Release \
         --target OpenDoctrines -j "$jobs"

# The binary is in a different place on each platform, and every step below
# needs it, so resolve it once and say so.
game=""
for candidate in "$build/OpenDoctrines.app/Contents/MacOS/OpenDoctrines" \
                 "$build/OpenDoctrines" "$build/Release/OpenDoctrines.exe" \
                 "$build/OpenDoctrines.exe"; do
    [ -x "$candidate" ] && { game="$candidate"; break; }
done
if [ -z "$game" ]; then
    bad "finding the built binary"
    printf '\nNothing was built, so nothing below could run.\n'
    exit 1
fi
note "binary: $game"

# ------------------------------------------------------------------ tests ---
run_step "the whole test suite" "$root/tests/run_all.sh" "$build"

# Already covered by run_all.sh, but run again on its own so a multiplayer
# failure is reported as a multiplayer failure rather than as "the suite".
run_step "four players, headless" "$root/tests/connectivity_test.sh" "$build"

# -------------------------------------------------- a real game, unattended ---
# The check nothing above makes: can this build actually load a shipped
# scenario and resolve turns? Five turns is enough to prove the pipeline and
# short enough not to dominate the run.
step "play a real game (5 turns of 1939)"
sim_out="$build/qualify-sim"
rm -rf "$sim_out" && mkdir -p "$sim_out/saves"
for entry in "$root/data"/*; do
    base="$(basename "$entry")"
    case "$base" in account.json|config.json|servers.json|saves|mods|mods.json) continue ;; esac
    [ -e "$sim_out/$base" ] || ln -s "$entry" "$sim_out/$base"
done
echo '{"musicVolume":0,"accountIssuer":""}' > "$sim_out/config.json"

# ${runner[@]+"${runner[@]}"}, not "${runner[@]}". Under `set -u` bash 3.2
# treats expanding an EMPTY array as an unbound variable and dies -- and macOS
# ships bash 3.2, including on both macOS CI runners. So the plain form works
# everywhere the array is non-empty (Linux, under xvfb) and fails on exactly
# the platforms where it is empty. This idiom expands to nothing when unset.
if OD_DATA_DIR="$sim_out" ${runner[@]+"${runner[@]}"} "$game" \
       --simulate "$root/data/STDmaps/1939.odmap" 5 "Qualify" 2>&1 | grep -E '^\[SIM\]'; then
    if [ -f "$sim_out/saves/Qualify.odsv" ]; then
        turns=$(python3 - "$sim_out/saves/Qualify.odsv" <<'PY'
import sys, zipfile
try:
    z = zipfile.ZipFile(sys.argv[1])
    print(len([n for n in z.namelist() if n.startswith("turns/t_")]))
except Exception:
    print(0)
PY
)
        if [ "${turns:-0}" -ge 2 ]; then
            note "ok -- $turns turns resolved and saved"
        else
            bad "play a real game (only $turns turn(s) in the save)"
        fi
    else
        bad "play a real game (no save was written)"
    fi
else
    bad "play a real game (the simulation did not run)"
fi

# ----------------------------------------------------------------- verdict ---
printf '\n'
if [ "$fail" -eq 0 ]; then
    printf '\033[32m%s QUALIFIED\033[0m -- built, tested, and played a game.\n' "$platform"
else
    printf '\033[31m%s NOT QUALIFIED\033[0m. What failed:\n' "$platform"
    for s in "${failed_steps[@]}"; do printf '  - %s\n' "$s"; done
fi
exit "$fail"
