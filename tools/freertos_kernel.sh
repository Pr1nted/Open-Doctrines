#!/bin/sh
# Fetch the FreeRTOS kernel the FreeRTOS edition is built and tested against.
#
#   tools/freertos_kernel.sh [DIR]        (default: build/FreeRTOS-Kernel)
#
# Pinned by tag AND commit: a tag can be moved, a commit cannot, and a kernel
# that changed underneath this build would change the scheduler it was tested
# on without anything here saying so. MIT-licensed; it is fetched, not vendored.
set -eu
TAG=V11.1.0
COMMIT=dbf70559b27d39c1fdb68dfb9a32140b6a6777a0
root="$(cd "$(dirname "$0")/.." && pwd)"
dir="${1:-$root/build/FreeRTOS-Kernel}"
if [ ! -f "$dir/tasks.c" ]; then
    rm -rf "$dir"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$TAG" https://github.com/FreeRTOS/FreeRTOS-Kernel.git "$dir"
fi
got="$(git -C "$dir" rev-parse HEAD)"
if [ "$got" != "$COMMIT" ]; then
    echo "FAILED: $dir is at $got, not $TAG ($COMMIT)" >&2
    exit 1
fi
echo "FreeRTOS kernel $TAG at $dir"
