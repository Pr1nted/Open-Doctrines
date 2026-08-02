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
            clang lld \
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
        #
        # clang is for the MOD tests, not for building the game: the fixture
        # mods are compiled to wasm32, and without a wasm32-capable compiler
        # tests/build_test_mods.sh prints "no wasm32-capable clang found;
        # skipping" and the whole mod half of the suite quietly does not run.
        # A skip that reads like a pass is worse than a failure.
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

for tool in cmake node; do
    command -v "$tool" >/dev/null || bad "$tool is not installed"
done

# python is checked by RUNNING one, not by finding the name. `command -v python3`
# was the old test and it passes against the python3.exe stub Windows keeps in
# WindowsApps -- which prints an advertisement for the Microsoft Store and exits.
# So this step said the toolchain was fine and six checks in the suite then
# failed, on a machine with a perfectly good Python installed as `python`.
if od_py="$("$root/tools/find_python.sh" 2>/dev/null)"; then
    note "python: $od_py"
else
    bad "a working python3 (the name may resolve to a stub that is not python)"
fi
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

# OD_CMAKE_ARGS is passed straight through to configure. It exists so the Intel
# macOS build can be qualified on an Apple Silicon machine without a second
# computer:
#
#   OD_CMAKE_ARGS=-DCMAKE_OSX_ARCHITECTURES=x86_64 tools/qualify.sh build-x64
#
# clang is universal, so that cross-compiles, and Rosetta runs the resulting
# x86_64 test binaries. Unquoted on purpose: this is a list of flags, and the one
# use it has needs word splitting.
run_step "configure" cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release \
         ${OD_CMAKE_ARGS:-}
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

# Software OpenGL, for a machine with no GPU of its own.
#
# Set OD_SOFTWARE_GL_DIR to a directory of OpenGL DLLs -- Mesa's x64 payload --
# and they are copied beside the game before the window step below. Windows
# resolves a DLL next to the executable before the system copy, so this replaces
# opengl32.dll FOR THIS PROCESS ONLY: nothing is installed and nothing outside
# the build directory is touched.
#
# It exists because Windows was the one platform where the window step could
# never run. A hosted runner has no GPU, WGL refuses, and the step below
# correctly reports SKIPPED -- which left CI proving the Windows build compiled
# and never proving it starts. That is precisely the gap v1.0.3a shipped
# through: an executable that could not start at all, on the only platform
# whose "does it run" check was permanently skipped.
#
# Unset, everything behaves exactly as before. This is a TEST aid; none of it
# is packaged or shipped.
if [ -n "${OD_SOFTWARE_GL_DIR:-}" ]; then
    if [ -d "$OD_SOFTWARE_GL_DIR" ]; then
        cp "$OD_SOFTWARE_GL_DIR"/*.dll "$(dirname "$game")/" 2>/dev/null \
            && note "software OpenGL: copied beside the game from $OD_SOFTWARE_GL_DIR" \
            || note "software OpenGL: no DLLs copied from $OD_SOFTWARE_GL_DIR"
    else
        note "OD_SOFTWARE_GL_DIR is set but is not a directory: $OD_SOFTWARE_GL_DIR"
    fi
fi

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
# Output goes to a FILE, not through a pipe to grep. Piping meant the exit
# status was grep's, so a game that printed an error and died produced no
# [SIM] lines, grep failed, and the step reported "the simulation did not run"
# -- which is the one thing the log then could not tell you. The whole point of
# this step is to catch a build that does not run; hiding why is the worst
# possible way for it to fail.
sim_log="$build/qualify-sim.log"
OD_DATA_DIR="$sim_out" ${runner[@]+"${runner[@]}"} "$game" \
    --simulate "$root/data/STDmaps/1939.odmap" 5 "Qualify" > "$sim_log" 2>&1
sim_rc=$?
grep -E '^\[SIM\]' "$sim_log" || true
# "This machine has no display" and "this build is broken" are different
# answers and must not be reported as the same one. A hosted macOS runner has
# no usable GL context -- GLFW says "Failed to find a suitable pixel format" --
# and there is no xvfb for a Cocoa app, so this check simply cannot run there.
#
# It is a SKIP, and a loud one: the step is still printed, still says what it
# could not do, and the other three platforms still run it for real. Silently
# passing would turn the one check that proves a build runs into a check that
# proves nothing.
# Each platform words "there is no GPU here" differently, and the list has to
# carry all of them or the step reports a broken build on a machine that simply
# cannot draw:
#
#   macOS    NSGL: Failed to find a suitable pixel format
#   Windows  WGL: The driver does not appear to support OpenGL
#   Linux    GLX / Failed to initialize GLFW  (usually avoided by xvfb)
#
# On Windows the process then SEGFAULTS rather than returning: raylib's
# InitWindow() crashes inside itself once WGL refuses, so Game::init()'s
# IsWindowReady() check never runs. That is why the log is matched rather than
# the exit code -- there is no clean exit to inspect.
if grep -qE 'suitable pixel format|does not appear to support OpenGL|could not open a window|Failed to initialize GLFW|GLX' "$sim_log"; then
    note "SKIPPED -- this machine has no usable display, so the game cannot"
    note "          open a window here. The build and every other check above"
    note "          still ran; only 'does it play' is unproven on this runner."
    echo "  --- what the game said ---"
    grep -E 'GLFW|could not open a window' "$sim_log" | head -4 | sed 's/^/  /'
elif [ "$sim_rc" -ne 0 ]; then
    bad "play a real game (the game exited $sim_rc)"
    echo "  --- last 25 lines of $sim_log ---"
    tail -25 "$sim_log" | sed 's/^/  /'
else
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
