#!/usr/bin/env bash
#
# Build and run AI self-play training, optimised.
#
#   tools/train.sh                  endless maps, default settings
#   tools/train.sh 4 2000           4 maps, 2000 turns each
#   tools/train.sh 0 3000 40 1234   maps turns countries seed (0 maps = endless)
#
# Why this exists rather than "just run the binary you built":
#
#   The IDE's default configuration (cmake-build-debug) compiles with -g and NO
#   optimisation. Only src/ai/NeuralNet.cpp carries its own -O3, so every other
#   line of the simulation runs unoptimised there -- measured at 0.43 s/turn
#   against 0.063 s/turn for the same code built Release. Training out of the
#   debug directory throws away a factor of seven before it starts.
#
#   This script always builds and runs the Release configuration, so what you
#   train with is what you profiled.
#
# In-game: F10 or Ctrl+L opens the resource limiter (slider + CPU graph).
# Close the window to stop; the model is checkpointed once a minute and on exit.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build-release"
bin="$build/OpenDoctrines.app/Contents/MacOS/OpenDoctrines"

# Configure on first use only; reconfiguring every run would rebuild the world.
if [ ! -f "$build/CMakeCache.txt" ]; then
    echo "==> configuring Release build in $build"
    cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release
fi

jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
echo "==> building (Release, -j$jobs)"
cmake --build "$build" --target OpenDoctrines -j"$jobs"

echo "==> training: ${*:-endless maps}"
exec "$bin" --train-ai "$@"
