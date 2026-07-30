#!/usr/bin/env bash
# Qualify a platform inside a container, on any machine with docker.
#
#   tools/qualify_docker.sh              # linux, this machine's architecture
#   tools/qualify_docker.sh --amd64      # linux/amd64 (emulated on arm64: slow)
#
# WHY THIS EXISTS
#
# GitHub Actions stopped starting jobs on a spending limit, and this is a private
# repo where macOS bills at 10x. This is the part of the matrix that costs
# nothing to run: docker on a laptop, no account, no service, no minutes.
#
# It is also not merely a fallback. On a bare ubuntu:22.04 this produced FULLER
# coverage than either hosted macOS runner ever did -- Ubuntu's clang can target
# wasm32, so the fixture mods build and ModRuntimeTest runs its whole suite
# rather than the single check it managed on macOS, and xvfb means the five-turn
# simulation actually plays instead of skipping for want of a GL context.
#
# WHAT IT CANNOT COVER, AND WHY THAT MATTERS
#
# Linux. NOT macOS: there is no macOS container image and no legal way to make
# one. NOT Windows: Windows containers need a Windows host, so they cannot run
# here at all.
#
# And NOT the web build, deliberately. emscripten is a cross-compiler -- its
# output does not depend on the host -- so a container isolates nothing there,
# and putting it in one only added a memory ceiling: wasm-opt was SIGKILLed
# twice inside emscripten/emsdk (a native arm64 image, so not emulation), while
# the same build succeeds on the host. Build it directly instead:
#
#   emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release
#   emmake cmake --build build-web -j 6
#
# then check the four artifacts and data/audio/music, which is what
# .github/workflows/test.yml does.
#
# Those two are exactly where the last two real bugs were -- WAMR's fuel limit
# silently absent under MSVC, and pack_odmod.sh needing a `zip` binary Git Bash
# does not ship. Neither would have been caught by anything in this file. Treat
# it as a way to keep Linux honest for free, never as the four-platform matrix.
#
# NOTHING LANDS IN YOUR TREE
#
# The repo is mounted READ-ONLY and the build directory is a separate mount, so a
# container run cannot leave build artefacts, installed packages or a half-
# written save behind. That is also what makes it safe to run while you have work
# in progress.
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
platform=""

while [ $# -gt 0 ]; do
    case "$1" in
        --amd64) platform="--platform=linux/amd64" ;;
        --arm64) platform="--platform=linux/arm64" ;;
        -h|--help) sed -n '2,5p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

command -v docker >/dev/null 2>&1 || {
    echo "docker is not installed, or not on PATH." >&2
    exit 1
}
docker info >/dev/null 2>&1 || {
    echo "docker is installed but the daemon is not responding." >&2
    echo "  Start Docker Desktop (or your engine of choice) and try again." >&2
    exit 1
}

# Kept between runs on purpose. A cold configure fetches WAMR and mbedTLS and
# then builds the world; throwing that away every time would make this too slow
# to reach for, and the point is that it is cheap enough to actually use.
out="$root/.docker-build/linux${platform:+-amd64}"
mkdir -p "$out"

# OD_JOBS, not the core count. This project is memory-hungry to compile and the
# core count is a bad proxy for how many translation units fit in RAM at once --
# a container given 8 GB and eight cores gets its build OOM-killed, which looks
# exactly like a compiler crash. Docker Desktop's default allowance is small.
jobs="${OD_JOBS:-2}"


echo "=== linux, in a container ==="
[ -n "$platform" ] && echo "  ${platform#--platform=} (emulated builds are several times slower)"
echo "  build dir: $out"

# ubuntu:22.04 to match what the workflow targets, and deliberately BARE:
# tools/qualify.sh installs everything it needs itself -- compilers, node from
# NodeSource, python3, Pillow, the raylib X11/ALSA packages and xvfb. Installing
# them here instead would be a second dependency list to keep in step with the
# one qualify.sh already has, and the whole point of that script is that there
# is only one.
#
# apt-get update runs first only because the image ships no package lists at all,
# so qualify.sh's own install would fail on a cold image before it could report
# anything useful.
exec docker run --rm $platform \
    -v "$root":/repo:ro -v "$out":/build -w /repo \
    -e OD_JOBS="$jobs" -e OD_ROOT=/repo \
    ubuntu:22.04 bash -c 'apt-get update -qq >/dev/null 2>&1; tools/qualify.sh /build'
